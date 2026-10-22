/// @file auth_manager.cpp
/// @brief Implementation of web session auth + API-key auth.
///
/// Ported from sslivins/arctic-controller (auth_manager.cpp), trimmed to the
/// pieces this firmware needs.
///
/// Admin-claim model:
///  - LEGACY: no admin password and no claim pending (a head upgraded from
///    older firmware). The UI stays open, with a banner asking for a password.
///  - UNCLAIMED: no admin password but a claim is pending (fresh
///    provisioning). Everything except the public auth endpoints is locked
///    until the password is created with the claim token from the portal, or
///    within the recovery window.
///  - CLAIMED: admin password set; web login is always enforced.
/// The reset procedure opens a 15-minute recovery window in which anyone may
/// set a new admin password; if nobody does, nothing changes.
#include "auth_manager.h"

#include <mutex>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "psa/crypto.h"
#include "token_store.h"

static const char* TAG = "auth_mgr";

// Monotonic seconds since boot. Session lifetimes MUST use this rather than
// time(NULL): the wall clock jumps forward when SNTP syncs (typically a few
// seconds after an OTA reboot), which would otherwise instantly "expire" every
// session created before the sync and log the user straight back out.
static time_t monotonic_now(void) {
    return (time_t)(esp_timer_get_time() / 1000000);
}

// NVS namespace and keys
static const char* NVS_NAMESPACE = "auth";
static const char* NVS_KEY_WEB_ENABLED = "web_en";   // derived; kept for old firmware
static const char* NVS_KEY_API_ENABLED = "api_en";
static const char* NVS_KEY_USERNAME = "username";
static const char* NVS_KEY_PASS_HASH = "pass_hash";  // legacy unsalted SHA-256
static const char* NVS_KEY_USER_HASH = "user_hash";  // legacy unsalted SHA-256
static const char* NVS_KEY_PASS_V2 = "pass_v2";      // pw_rec_t (PBKDF2)
static const char* NVS_KEY_USER_V2 = "user_v2";      // pw_rec_t (PBKDF2)
static const char* NVS_KEY_API_KEY = "api_key";
static const char* NVS_KEY_CLAIM_REQ = "claim_req";
static const char* NVS_KEY_CLAIM_TOK = "claim_tok";
static const char* NVS_KEY_ADMIN_EPOCH = "adm_ep";   // bumps on each admin password change

// Password verifier record. ver 0 = unset, 1 = legacy unsalted SHA-256 (read
// from old NVS keys, upgraded on the next successful login), 2 = salted
// PBKDF2-HMAC-SHA256.
#define PW_VER_NONE 0
#define PW_VER_SHA256 1
#define PW_VER_PBKDF2 2
#define PBKDF2_ITERS 4096
#define PW_SALT_LEN 16

typedef struct {
    uint8_t ver;
    uint8_t rsv[3];
    uint32_t iters;
    uint8_t salt[PW_SALT_LEN];
    uint8_t hash[32];
} pw_rec_t;
static_assert(sizeof(pw_rec_t) == AUTH_ADMIN_REC_LEN, "verifier record layout");

// Failed-login throttle: after this many consecutive failures, refuse logins
// for AUTH_LOCKOUT_SEC.
#define AUTH_MAX_FAILS 10
#define AUTH_LOCKOUT_SEC 30

typedef struct {
    bool active;
    char token[AUTH_SESSION_TOKEN_LEN + 1];
    auth_role_t role;
    time_t created_at;
    time_t last_used;
} session_t;

static struct {
    bool initialized;
    bool api_auth_enabled;
    char username[AUTH_MAX_USERNAME_LEN + 1];
    pw_rec_t admin;   // admin password verifier
    uint32_t admin_epoch;  // group sync: newer epoch wins (see auth_mgr_adopt_admin)
    pw_rec_t user;    // shared climate-only user password verifier
    bool claim_required;
    char claim_token[AUTH_CLAIM_TOKEN_LEN + 1];
    time_t recovery_until;  // monotonic; 0 = no recovery window
    int fail_count;
    time_t lockout_until;   // monotonic
    char api_key[AUTH_API_KEY_LEN + 1];
    session_t sessions[AUTH_MAX_SESSIONS];
} state = {};

// Recovery can be triggered from the MQTT/PMIC tasks while the httpd task is
// using the auth state, so every public entry point takes this lock.
static std::recursive_mutex s_mtx;
#define AUTH_LOCK() std::lock_guard<std::recursive_mutex> _auth_guard(s_mtx)

// ── Helpers ────────────────────────────────────────────────────────────────

static void generate_random_hex(char* buffer, size_t len) {
    static const char hex_chars[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        buffer[i] = hex_chars[esp_random() % 16];
    }
    buffer[len] = '\0';
}

static bool ct_equal(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
    return d == 0;
}

static bool ct_equal_str(const char* a, const char* b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la != lb || la == 0) return false;
    return ct_equal((const uint8_t*)a, (const uint8_t*)b, la);
}

static void sha256_password(const char* password, uint8_t* hash_out) {
    size_t hash_len = 0;
    if (psa_crypto_init() != PSA_SUCCESS ||
        psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t*)password, strlen(password),
                         hash_out, 32, &hash_len) != PSA_SUCCESS) {
        ESP_LOGE(TAG, "SHA-256 failed");
        memset(hash_out, 0, 32);
    }
}

// PBKDF2-HMAC-SHA256, single 32-byte output block (RFC 8018), built on PSA HMAC.
static bool pbkdf2_sha256(const char* password, const uint8_t* salt, size_t salt_len,
                          uint32_t iters, uint8_t out[32]) {
    size_t pw_len = strlen(password);
    if (pw_len == 0 || iters == 0 || psa_crypto_init() != PSA_SUCCESS) return false;

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_key_id_t key = 0;
    if (psa_import_key(&attr, (const uint8_t*)password, pw_len, &key) != PSA_SUCCESS) {
        ESP_LOGE(TAG, "PBKDF2 key import failed");
        return false;
    }

    uint8_t block[PW_SALT_LEN + 4];
    if (salt_len > PW_SALT_LEN) salt_len = PW_SALT_LEN;
    memcpy(block, salt, salt_len);
    block[salt_len + 0] = 0;
    block[salt_len + 1] = 0;
    block[salt_len + 2] = 0;
    block[salt_len + 3] = 1;  // INT(1): first (only) output block

    uint8_t u[32];
    size_t olen = 0;
    bool ok = psa_mac_compute(key, PSA_ALG_HMAC(PSA_ALG_SHA_256), block, salt_len + 4,
                              u, sizeof(u), &olen) == PSA_SUCCESS;
    if (ok) memcpy(out, u, 32);
    for (uint32_t i = 1; ok && i < iters; i++) {
        ok = psa_mac_compute(key, PSA_ALG_HMAC(PSA_ALG_SHA_256), u, sizeof(u),
                             u, sizeof(u), &olen) == PSA_SUCCESS;
        for (int j = 0; j < 32; j++) out[j] ^= u[j];
    }
    psa_destroy_key(key);
    memset(u, 0, sizeof(u));
    if (!ok) {
        ESP_LOGE(TAG, "PBKDF2 failed");
        memset(out, 0, 32);
    }
    return ok;
}

static bool rec_make(const char* password, pw_rec_t* rec) {
    pw_rec_t r = {};
    r.ver = PW_VER_PBKDF2;
    r.iters = PBKDF2_ITERS;
    esp_fill_random(r.salt, sizeof(r.salt));
    int64_t t0 = esp_timer_get_time();
    if (!pbkdf2_sha256(password, r.salt, sizeof(r.salt), r.iters, r.hash)) return false;
    ESP_LOGI(TAG, "password hashed (PBKDF2 x%u, %lld ms)", (unsigned)r.iters,
             (long long)((esp_timer_get_time() - t0) / 1000));
    *rec = r;
    return true;
}

static bool rec_matches(const pw_rec_t* rec, const char* password) {
    if (password == NULL || password[0] == '\0') return false;
    uint8_t h[32];
    bool ok = false;
    if (rec->ver == PW_VER_SHA256) {
        sha256_password(password, h);
        ok = ct_equal(h, rec->hash, 32);
    } else if (rec->ver == PW_VER_PBKDF2) {
        ok = pbkdf2_sha256(password, rec->salt, sizeof(rec->salt), rec->iters, h) &&
             ct_equal(h, rec->hash, 32);
    }
    memset(h, 0, sizeof(h));
    return ok;
}

static bool load_rec(nvs_handle_t nvs, const char* v2_key, const char* legacy_key,
                     pw_rec_t* rec) {
    memset(rec, 0, sizeof(*rec));
    size_t len = sizeof(*rec);
    if (nvs_get_blob(nvs, v2_key, rec, &len) == ESP_OK && len == sizeof(*rec) &&
        rec->ver == PW_VER_PBKDF2 && rec->iters > 0)
        return true;
    memset(rec, 0, sizeof(*rec));
    len = sizeof(rec->hash);
    if (nvs_get_blob(nvs, legacy_key, rec->hash, &len) == ESP_OK && len == sizeof(rec->hash)) {
        rec->ver = PW_VER_SHA256;
        return true;
    }
    memset(rec, 0, sizeof(*rec));
    return false;
}

static bool load_from_nvs(void) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        ESP_LOGI(TAG, "no auth settings in NVS, using defaults");
        return false;
    }

    uint8_t u8 = 0;
    if (nvs_get_u8(nvs, NVS_KEY_API_ENABLED, &u8) == ESP_OK)
        state.api_auth_enabled = u8 != 0;
    if (nvs_get_u8(nvs, NVS_KEY_CLAIM_REQ, &u8) == ESP_OK)
        state.claim_required = u8 != 0;
    uint32_t u32 = 0;
    if (nvs_get_u32(nvs, NVS_KEY_ADMIN_EPOCH, &u32) == ESP_OK) state.admin_epoch = u32;

    size_t len = sizeof(state.username);
    if (nvs_get_str(nvs, NVS_KEY_USERNAME, state.username, &len) != ESP_OK)
        state.username[0] = '\0';

    load_rec(nvs, NVS_KEY_PASS_V2, NVS_KEY_PASS_HASH, &state.admin);
    load_rec(nvs, NVS_KEY_USER_V2, NVS_KEY_USER_HASH, &state.user);

    len = sizeof(state.claim_token);
    if (nvs_get_str(nvs, NVS_KEY_CLAIM_TOK, state.claim_token, &len) != ESP_OK)
        state.claim_token[0] = '\0';

    len = sizeof(state.api_key);
    if (nvs_get_str(nvs, NVS_KEY_API_KEY, state.api_key, &len) != ESP_OK)
        state.api_key[0] = '\0';

    nvs_close(nvs);
    return true;
}

// Persist a password record under its v2 key (PBKDF2) or legacy key (SHA-256),
// erasing the other form so exactly one survives.
static void save_rec(nvs_handle_t nvs, const pw_rec_t* rec, const char* v2_key,
                     const char* legacy_key, esp_err_t& e) {
    auto acc = [&](esp_err_t r) { if (r != ESP_OK && e == ESP_OK) e = r; };
    auto erase = [&](const char* k) {
        esp_err_t r = nvs_erase_key(nvs, k);
        if (r != ESP_ERR_NVS_NOT_FOUND) acc(r);
    };
    if (rec->ver == PW_VER_PBKDF2) {
        acc(nvs_set_blob(nvs, v2_key, rec, sizeof(*rec)));
        erase(legacy_key);
    } else if (rec->ver == PW_VER_SHA256) {
        acc(nvs_set_blob(nvs, legacy_key, rec->hash, sizeof(rec->hash)));
        erase(v2_key);
    } else {
        erase(v2_key);
        erase(legacy_key);
    }
}

// Persist the whole auth state. Returns ESP_OK only if the write AND commit
// both succeed — callers MUST treat a non-OK result as "not persisted".
static esp_err_t save_to_nvs(void) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    // erase_key returns ESP_ERR_NVS_NOT_FOUND when the key is absent — that is a
    // benign no-op, not a failure, so it is excluded from the error accumulator.
    esp_err_t e = ESP_OK;
    auto acc = [&](esp_err_t r) { if (r != ESP_OK && e == ESP_OK) e = r; };

    acc(nvs_set_u8(nvs, NVS_KEY_WEB_ENABLED, auth_mgr_web_auth_enabled() ? 1 : 0));
    acc(nvs_set_u8(nvs, NVS_KEY_API_ENABLED, state.api_auth_enabled ? 1 : 0));
    acc(nvs_set_u8(nvs, NVS_KEY_CLAIM_REQ, state.claim_required ? 1 : 0));
    acc(nvs_set_u32(nvs, NVS_KEY_ADMIN_EPOCH, state.admin_epoch));
    if (state.username[0] != '\0')
        acc(nvs_set_str(nvs, NVS_KEY_USERNAME, state.username));
    save_rec(nvs, &state.admin, NVS_KEY_PASS_V2, NVS_KEY_PASS_HASH, e);
    save_rec(nvs, &state.user, NVS_KEY_USER_V2, NVS_KEY_USER_HASH, e);
    if (state.claim_token[0] != '\0') {
        acc(nvs_set_str(nvs, NVS_KEY_CLAIM_TOK, state.claim_token));
    } else {
        esp_err_t r = nvs_erase_key(nvs, NVS_KEY_CLAIM_TOK);
        if (r != ESP_ERR_NVS_NOT_FOUND) acc(r);
    }
    if (state.api_key[0] != '\0')
        acc(nvs_set_str(nvs, NVS_KEY_API_KEY, state.api_key));

    acc(nvs_commit(nvs));
    nvs_close(nvs);
    if (e != ESP_OK) ESP_LOGE(TAG, "auth NVS save failed: %s", esp_err_to_name(e));
    return e;
}

static session_t* find_session(const char* token) {
    if (token == NULL || token[0] == '\0') return NULL;
    for (int i = 0; i < AUTH_MAX_SESSIONS; i++) {
        if (state.sessions[i].active && strcmp(state.sessions[i].token, token) == 0)
            return &state.sessions[i];
    }
    return NULL;
}

// Find a slot for a new session of @p new_role. A free slot is always preferred;
// otherwise the oldest evictable session is reclaimed. A climate-only USER login
// must never evict an ADMIN session — doing so could knock the only admin out
// while the admin password is forgotten (a lockout vector), so user logins only
// reclaim other user slots and are refused when none are available.
static session_t* find_free_session(auth_role_t new_role) {
    session_t* oldest = NULL;
    time_t oldest_time = 0;
    for (int i = 0; i < AUTH_MAX_SESSIONS; i++) {
        if (!state.sessions[i].active) return &state.sessions[i];
        bool evictable = (new_role == AUTH_ROLE_ADMIN) ||
                         (state.sessions[i].role != AUTH_ROLE_ADMIN);
        if (evictable && (oldest == NULL || state.sessions[i].last_used < oldest_time)) {
            oldest = &state.sessions[i];
            oldest_time = state.sessions[i].last_used;
        }
    }
    if (oldest != NULL) {  // all slots full → evict oldest evictable session
        oldest->active = false;
        return oldest;
    }
    return NULL;
}

static bool create_session(auth_role_t role, char* session_token) {
    session_t* session = find_free_session(role);
    if (session == NULL) return false;
    session->active = true;
    session->role = role;
    generate_random_hex(session->token, AUTH_SESSION_TOKEN_LEN);
    session->created_at = monotonic_now();
    session->last_used = session->created_at;
    if (session_token != NULL) strcpy(session_token, session->token);
    return true;
}

static bool is_session_expired(const session_t* session) {
    if (session == NULL || !session->active) return true;
    return (monotonic_now() - session->created_at) > AUTH_SESSION_LIFETIME_SEC;
}

static bool password_set(void) { return state.admin.ver != PW_VER_NONE; }

static bool recovery_open(void) {
    return state.recovery_until != 0 && monotonic_now() < state.recovery_until;
}

// ── Public API ───────────────────────────────────────────────────────────

void auth_mgr_init(void) {
    AUTH_LOCK();
    if (state.initialized) return;

    memset(&state, 0, sizeof(state));
    load_from_nvs();
    bool dirty = false;

    // The admin username is fixed to "admin" and cannot be changed.
    if (strcmp(state.username, "admin") != 0) {
        strncpy(state.username, "admin", sizeof(state.username) - 1);
        state.username[sizeof(state.username) - 1] = '\0';
        dirty = true;
    }

    // Migration: older firmware auto-seeded admin/admin. If a device still
    // carries that exact default, clear it so there is no default password.
    if (state.admin.ver == PW_VER_SHA256 && rec_matches(&state.admin, "admin")) {
        memset(&state.admin, 0, sizeof(state.admin));
        dirty = true;
        ESP_LOGI(TAG, "cleared legacy default admin password");
    }

    if (state.api_key[0] == '\0') {
        generate_random_hex(state.api_key, AUTH_API_KEY_LEN);
        dirty = true;
    }

    // A pending claim is meaningless once a password exists.
    if (password_set() && (state.claim_required || state.claim_token[0] != '\0')) {
        state.claim_required = false;
        state.claim_token[0] = '\0';
        dirty = true;
    }

    if (dirty) save_to_nvs();
    state.initialized = true;
    ESP_LOGI(TAG, "auth ready — state=%s api_auth=%s",
             password_set() ? "claimed" : state.claim_required ? "unclaimed" : "legacy",
             state.api_auth_enabled ? "on" : "off");
}

auth_state_t auth_mgr_state(void) {
    AUTH_LOCK();
    if (password_set()) return AUTH_STATE_CLAIMED;
    return state.claim_required ? AUTH_STATE_UNCLAIMED : AUTH_STATE_LEGACY;
}

// ── Web auth ─────────────────────────────────────────────────────────────

// Login is enforced whenever an admin password exists, and while a claim is
// pending (so an unclaimed head exposes nothing but the claim flow).
bool auth_mgr_web_auth_enabled(void) {
    AUTH_LOCK();
    return password_set() || state.claim_required;
}

bool auth_mgr_set_admin_password(const char* password) {
    AUTH_LOCK();
    if (password == NULL || password[0] == '\0' || strlen(password) > AUTH_MAX_PASSWORD_LEN)
        return false;
    // The admin and user passwords must differ. Login is password-only and the
    // admin is matched first, so an identical user password would silently
    // resolve to admin (a privilege escalation). Reject the collision.
    if (rec_matches(&state.user, password)) return false;
    pw_rec_t r;
    if (!rec_make(password, &r)) return false;
    state.admin = r;
    state.admin_epoch++;
    state.claim_required = false;
    state.claim_token[0] = '\0';
    state.recovery_until = 0;
    if (save_to_nvs() != ESP_OK) return false;
    auth_mgr_logout_all();  // force re-login after a credential change
    return true;
}

bool auth_mgr_set_user_password(const char* password) {
    AUTH_LOCK();
    if (password == NULL || password[0] == '\0') {
        // Remove (disable) the shared climate-only user account.
        memset(&state.user, 0, sizeof(state.user));
    } else {
        if (strlen(password) > AUTH_MAX_PASSWORD_LEN) return false;
        // Must differ from the admin password (see auth_mgr_set_admin_password):
        // a user password equal to admin's would log the user in as admin.
        if (rec_matches(&state.admin, password)) return false;
        pw_rec_t r;
        if (!rec_make(password, &r)) return false;
        state.user = r;
    }
    save_to_nvs();
    // Invalidate any active user-role sessions so a removed/changed password
    // takes effect immediately.
    for (int i = 0; i < AUTH_MAX_SESSIONS; i++)
        if (state.sessions[i].active && state.sessions[i].role == AUTH_ROLE_USER)
            state.sessions[i].active = false;
    return true;
}

bool auth_mgr_user_password_set(void) {
    AUTH_LOCK();
    return state.user.ver != PW_VER_NONE;
}

const char* auth_mgr_get_username(void) { return state.username; }

bool auth_mgr_web_password_set(void) {
    AUTH_LOCK();
    return password_set();
}

int auth_mgr_login_lockout_secs(void) {
    AUTH_LOCK();
    time_t now = monotonic_now();
    return state.lockout_until > now ? (int)(state.lockout_until - now) : 0;
}

auth_role_t auth_mgr_login(const char* password, char* session_token) {
    AUTH_LOCK();
    if (!auth_mgr_web_auth_enabled()) return AUTH_ROLE_ADMIN;  // legacy open → full access
    if (password == NULL || password[0] == '\0') return AUTH_ROLE_NONE;
    if (auth_mgr_login_lockout_secs() > 0) return AUTH_ROLE_NONE;

    auth_role_t role = AUTH_ROLE_NONE;
    pw_rec_t* matched = NULL;
    if (rec_matches(&state.admin, password)) {
        role = AUTH_ROLE_ADMIN;  // admin checked first; "admin" is reserved
        matched = &state.admin;
    } else if (rec_matches(&state.user, password)) {
        role = AUTH_ROLE_USER;
        matched = &state.user;
    }
    if (role == AUTH_ROLE_NONE) {
        auth_mgr_note_failed_login();
        return AUTH_ROLE_NONE;
    }
    state.fail_count = 0;

    // Upgrade a legacy unsalted hash now that we know the plaintext.
    if (matched->ver == PW_VER_SHA256) {
        pw_rec_t r;
        if (rec_make(password, &r)) {
            *matched = r;
            save_to_nvs();
            ESP_LOGI(TAG, "upgraded %s password hash to PBKDF2",
                     role == AUTH_ROLE_ADMIN ? "admin" : "user");
        }
    }

    if (!create_session(role, session_token)) return AUTH_ROLE_NONE;
    ESP_LOGI(TAG, "login ok, role=%s", role == AUTH_ROLE_ADMIN ? "admin" : "user");
    return role;
}

auth_role_t auth_mgr_session_role(const char* token) {
    AUTH_LOCK();
    if (!auth_mgr_web_auth_enabled()) return AUTH_ROLE_ADMIN;
    session_t* session = find_session(token);
    if (session == NULL) return AUTH_ROLE_NONE;
    if (is_session_expired(session)) {
        session->active = false;
        return AUTH_ROLE_NONE;
    }
    session->last_used = monotonic_now();
    return session->role;
}

bool auth_mgr_validate_session(const char* token) {
    return auth_mgr_session_role(token) != AUTH_ROLE_NONE;
}

void auth_mgr_logout(const char* token) {
    AUTH_LOCK();
    session_t* session = find_session(token);
    if (session != NULL) session->active = false;
}

void auth_mgr_logout_all(void) {
    AUTH_LOCK();
    for (int i = 0; i < AUTH_MAX_SESSIONS; i++) state.sessions[i].active = false;
}

// ── Claim / recovery ─────────────────────────────────────────────────────

bool auth_mgr_begin_claim(char* token_out) {
    AUTH_LOCK();
    if (password_set()) return false;
    state.claim_required = true;
    generate_random_hex(state.claim_token, AUTH_CLAIM_TOKEN_LEN);
    if (save_to_nvs() != ESP_OK) return false;
    if (token_out != NULL) strcpy(token_out, state.claim_token);
    ESP_LOGI(TAG, "claim token issued");
    return true;
}

auth_claim_result_t auth_mgr_claim(const char* password, const char* token,
                                   char* session_token) {
    AUTH_LOCK();
    // During a reset window the existing admin password may be replaced.
    const bool replacing = password_set();
    if (replacing && !recovery_open()) return AUTH_CLAIM_ALREADY;
    if (!replacing && state.claim_required) {
        bool token_ok = token != NULL && state.claim_token[0] != '\0' &&
                        ct_equal_str(token, state.claim_token);
        if (!token_ok && !recovery_open()) return AUTH_CLAIM_DENIED;
    }
    if (password == NULL || password[0] == '\0' || strlen(password) > AUTH_MAX_PASSWORD_LEN ||
        rec_matches(&state.user, password))
        return AUTH_CLAIM_INVALID;
    if (!auth_mgr_set_admin_password(password)) return AUTH_CLAIM_FAILED;
    if (replacing) {
        // Whoever knew the old password loses access: drop sessions, new API
        // key, revoke integration tokens.
        generate_random_hex(state.api_key, AUTH_API_KEY_LEN);
        save_to_nvs();
        auth_mgr_logout_all();
        token_store_revoke_all();
    }
    if (!create_session(AUTH_ROLE_ADMIN, session_token)) return AUTH_CLAIM_FAILED;
    ESP_LOGI(TAG, "admin password %s", replacing ? "replaced (reset procedure)"
                                                 : "created (claimed)");
    return AUTH_CLAIM_OK;
}

void auth_mgr_recover(void) {
    AUTH_LOCK();
    // Nothing changes unless a new admin password is set within the window.
    state.recovery_until = monotonic_now() + AUTH_RECOVERY_WINDOW_SEC;
    state.fail_count = 0;
    state.lockout_until = 0;
    ESP_LOGW(TAG, "admin password reset — window open for %d s", AUTH_RECOVERY_WINDOW_SEC);
}

int auth_mgr_recovery_secs_left(void) {
    AUTH_LOCK();
    if (!recovery_open()) return 0;
    return (int)(state.recovery_until - monotonic_now());
}

// ── Group join + admin-password sync ─────────────────────────────────────

void auth_mgr_note_failed_login(void) {
    AUTH_LOCK();
    if (++state.fail_count >= AUTH_MAX_FAILS) {
        state.fail_count = 0;
        state.lockout_until = monotonic_now() + AUTH_LOCKOUT_SEC;
        ESP_LOGW(TAG, "too many failed logins — locked for %d s", AUTH_LOCKOUT_SEC);
    }
}

void auth_mgr_note_good_login(void) {
    AUTH_LOCK();
    state.fail_count = 0;
}

bool auth_mgr_admin_verifier(int* ver, uint8_t salt[AUTH_SALT_LEN], uint32_t* iters,
                             uint8_t key[32]) {
    AUTH_LOCK();
    if (!password_set()) return false;
    if (ver) *ver = state.admin.ver;
    if (salt) memcpy(salt, state.admin.salt, AUTH_SALT_LEN);
    if (iters) *iters = state.admin.iters;
    if (key) memcpy(key, state.admin.hash, 32);
    return true;
}

bool auth_mgr_derive_key(const char* password, int ver, const uint8_t salt[AUTH_SALT_LEN],
                         uint32_t iters, uint8_t key[32]) {
    if (password == NULL || password[0] == '\0' || strlen(password) > AUTH_MAX_PASSWORD_LEN)
        return false;
    if (ver == PW_VER_SHA256) {
        sha256_password(password, key);
        return true;
    }
    if (ver != PW_VER_PBKDF2 || iters == 0 || iters > AUTH_MAX_ITERS) return false;
    return pbkdf2_sha256(password, salt, AUTH_SALT_LEN, iters, key);
}

uint32_t auth_mgr_admin_epoch(void) {
    AUTH_LOCK();
    return state.admin_epoch;
}

bool auth_mgr_admin_record(uint8_t rec[AUTH_ADMIN_REC_LEN]) {
    AUTH_LOCK();
    if (!password_set()) return false;
    memcpy(rec, &state.admin, AUTH_ADMIN_REC_LEN);
    return true;
}

bool auth_mgr_adopt_admin(const uint8_t rec[AUTH_ADMIN_REC_LEN], uint32_t epoch) {
    AUTH_LOCK();
    pw_rec_t r;
    memcpy(&r, rec, sizeof(r));
    if (!(r.ver == PW_VER_SHA256 || (r.ver == PW_VER_PBKDF2 && r.iters > 0 &&
                                     r.iters <= AUTH_MAX_ITERS)))
        return false;
    const bool changed = memcmp(&r, &state.admin, sizeof(r)) != 0;
    if (!changed && epoch == state.admin_epoch) return false;
    state.admin = r;
    state.admin_epoch = epoch;
    state.claim_required = false;
    state.claim_token[0] = '\0';
    if (changed) state.recovery_until = 0;
    save_to_nvs();
    if (changed) auth_mgr_logout_all();
    ESP_LOGI(TAG, "admin password adopted from group (epoch %u)", (unsigned)epoch);
    return changed;
}

bool auth_mgr_join_authorized(const char* token) {
    AUTH_LOCK();
    if (recovery_open()) return true;
    return !password_set() && state.claim_required && token != NULL &&
           state.claim_token[0] != '\0' && ct_equal_str(token, state.claim_token);
}

bool auth_mgr_create_admin_session(char* session_token) {
    AUTH_LOCK();
    return create_session(AUTH_ROLE_ADMIN, session_token);
}

// ── API-key auth ─────────────────────────────────────────────────────────

bool auth_mgr_api_auth_enabled(void) {
    AUTH_LOCK();
    return state.api_auth_enabled;
}

void auth_mgr_set_api_auth_enabled(bool enabled) {
    AUTH_LOCK();
    if (state.api_auth_enabled == enabled) return;
    state.api_auth_enabled = enabled;
    save_to_nvs();
    ESP_LOGI(TAG, "API auth %s", enabled ? "enabled" : "disabled");
}

bool auth_mgr_get_api_key(char* buffer) {
    AUTH_LOCK();
    if (buffer == NULL) return false;
    if (state.api_key[0] == '\0') { buffer[0] = '\0'; return false; }
    strcpy(buffer, state.api_key);
    return true;
}

bool auth_mgr_regenerate_api_key(char* buffer) {
    AUTH_LOCK();
    generate_random_hex(state.api_key, AUTH_API_KEY_LEN);
    save_to_nvs();
    if (buffer != NULL) strcpy(buffer, state.api_key);
    ESP_LOGI(TAG, "API key regenerated");
    return true;
}

bool auth_mgr_validate_api_key(const char* key) {
    AUTH_LOCK();
    if (!state.api_auth_enabled) return true;
    if (key == NULL || key[0] == '\0') return false;
    return ct_equal_str(key, state.api_key);
}
