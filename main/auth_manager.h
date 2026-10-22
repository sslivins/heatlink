/// @file auth_manager.h
/// @brief Web-UI session auth + API-key auth for the on-device HTTP server.
///
/// Ported from sslivins/arctic-controller. Stores salted PBKDF2 password
/// verifiers and a random API key in NVS. Web auth uses random session tokens
/// carried in an HttpOnly cookie; API auth uses an X-API-Key header. Web login
/// is enforced whenever an admin password exists (or a claim is pending); API
/// auth is a separate toggle that defaults OFF.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Session token length (32 hex chars = 128 bits)
#define AUTH_SESSION_TOKEN_LEN 32
// API key length (32 hex chars)
#define AUTH_API_KEY_LEN 32
// Maximum concurrent sessions
#define AUTH_MAX_SESSIONS 4
// Session lifetime in seconds (7 days)
#define AUTH_SESSION_LIFETIME_SEC (7 * 24 * 60 * 60)
// Maximum username length
#define AUTH_MAX_USERNAME_LEN 32
// Maximum password length
#define AUTH_MAX_PASSWORD_LEN 64
// Claim token length (32 hex chars = 128 bits), issued by the setup portal
#define AUTH_CLAIM_TOKEN_LEN 32
// How long the admin-password reset procedure leaves the head claimable
#define AUTH_RECOVERY_WINDOW_SEC (15 * 60)
// Admin verifier record (version, iterations, salt, hash) as shared in a group
#define AUTH_ADMIN_REC_LEN 56
#define AUTH_SALT_LEN 16
// Upper bound on PBKDF2 iterations accepted from another head
#define AUTH_MAX_ITERS 100000

/// Access role attached to a web session. The "user" role is a shared,
/// climate-only login; "admin" has full access. Roles only apply to web
/// sessions — an API key or a legacy (open) device is always treated as
/// admin.
typedef enum {
    AUTH_ROLE_NONE = 0,
    AUTH_ROLE_USER = 1,
    AUTH_ROLE_ADMIN = 2,
} auth_role_t;

/// Admin-claim state of the head.
typedef enum {
    AUTH_STATE_LEGACY = 0,     ///< no admin password, no claim pending: UI open
    AUTH_STATE_UNCLAIMED = 1,  ///< no admin password, claim pending: UI locked
    AUTH_STATE_CLAIMED = 2,    ///< admin password set: login enforced
} auth_state_t;

typedef enum {
    AUTH_CLAIM_OK = 0,
    AUTH_CLAIM_ALREADY,  ///< an admin password is set and no recovery window is open
    AUTH_CLAIM_DENIED,   ///< claim token wrong/missing and no recovery window
    AUTH_CLAIM_INVALID,  ///< empty/too long, or equal to the user password
    AUTH_CLAIM_FAILED,   ///< couldn't hash/persist/create a session
} auth_claim_result_t;

/// Initialize the auth manager (loads settings from NVS). Safe to call once;
/// requires nvs_flash_init() to have already run.
void auth_mgr_init(void);

// ── Web authentication (password + session cookie) ─────────────────────────

auth_state_t auth_mgr_state(void);

/// True when web login is enforced: an admin password is set, or a claim is
/// pending. Not user-toggleable.
bool auth_mgr_web_auth_enabled(void);

/// Change the admin password (salted PBKDF2). The admin username is fixed to
/// "admin". All sessions are invalidated on change. Fails if the password is
/// empty/too long or equals the user password.
bool auth_mgr_set_admin_password(const char* password);

/// Whether the admin password is set.
bool auth_mgr_web_password_set(void);

/// Set (or, with NULL/empty, remove) the shared climate-only "user" password.
/// Existing user-role sessions are invalidated on change.
bool auth_mgr_set_user_password(const char* password);

/// Whether the climate-only "user" account is enabled (password set).
bool auth_mgr_user_password_set(void);

const char* auth_mgr_get_username(void);

/// Password-only login: the password is matched against the admin then the
/// user account. Returns the matched role (AUTH_ROLE_NONE on failure). On
/// success a session is created and its token written to @p session_token
/// (buffer must be AUTH_SESSION_TOKEN_LEN+1).
auth_role_t auth_mgr_login(const char* password, char* session_token);

/// Seconds left on the failed-login lockout (0 = logins allowed).
int auth_mgr_login_lockout_secs(void);

bool auth_mgr_validate_session(const char* token);

/// Role for a valid, unexpired session (AUTH_ROLE_NONE if invalid). Returns
/// AUTH_ROLE_ADMIN when web auth is disabled.
auth_role_t auth_mgr_session_role(const char* token);

void auth_mgr_logout(const char* token);
void auth_mgr_logout_all(void);

// ── Claim / recovery ───────────────────────────────────────────────────────

/// Mark the head as needing a claim and issue a fresh claim token (written to
/// @p token_out, AUTH_CLAIM_TOKEN_LEN+1). Returns false if an admin password is
/// already set. Called by the setup portal.
bool auth_mgr_begin_claim(char* token_out);

/// Set the admin password without logging in. Allowed while the recovery
/// window is open (replacing any existing password; sessions are dropped, the
/// API key rotated and integration tokens revoked), or when no password is set and either no claim is
/// pending (legacy head) or @p token matches the claim token. On success an
/// admin session is created.
auth_claim_result_t auth_mgr_claim(const char* password, const char* token,
                                   char* session_token);

/// Admin-password reset procedure: open the recovery window, during which
/// anyone can set a new admin password. Nothing else changes unless they do;
/// the existing admin password keeps working meanwhile.
void auth_mgr_recover(void);

/// Seconds left in the recovery window (0 = closed).
int auth_mgr_recovery_secs_left(void);

// ── Group join + admin-password sync ───────────────────────────────────────
// Heads on one outdoor unit share a single admin password. A head joins a group
// by proving it knows the password (HMAC keyed by the verifier hash), and the
// verifier record then gossips between members: a newer epoch wins.

/// Count a failed password attempt toward the login throttle.
void auth_mgr_note_failed_login(void);
/// Reset the failed-attempt counter after a successful password proof.
void auth_mgr_note_good_login(void);

/// This head's admin verifier: version (1 = SHA-256, 2 = PBKDF2), salt,
/// iterations and the 32-byte hash. False when no admin password is set.
bool auth_mgr_admin_verifier(int* ver, uint8_t salt[AUTH_SALT_LEN], uint32_t* iters,
                             uint8_t key[32]);

/// Derive the verifier hash for @p password under another head's parameters.
bool auth_mgr_derive_key(const char* password, int ver, const uint8_t salt[AUTH_SALT_LEN],
                         uint32_t iters, uint8_t key[32]);

/// Epoch of the admin password; bumps on every local change.
uint32_t auth_mgr_admin_epoch(void);

/// Raw admin verifier record. False when no admin password is set.
bool auth_mgr_admin_record(uint8_t rec[AUTH_ADMIN_REC_LEN]);

/// Replace the admin password with another head's verifier @p rec at
/// @p epoch. Completes a pending claim. Returns true if the password changed
/// (all sessions are then logged out).
bool auth_mgr_adopt_admin(const uint8_t rec[AUTH_ADMIN_REC_LEN], uint32_t epoch);

/// Whether a not-signed-in client may join a group: the recovery window is
/// open, or the head is unclaimed and @p token is its claim token.
bool auth_mgr_join_authorized(const char* token);

/// Create an admin session (token buffer AUTH_SESSION_TOKEN_LEN+1).
bool auth_mgr_create_admin_session(char* session_token);

// ── API-key authentication ─────────────────────────────────────────────────

bool auth_mgr_api_auth_enabled(void);
void auth_mgr_set_api_auth_enabled(bool enabled);

/// Copy the current API key into @p buffer (must be AUTH_API_KEY_LEN+1).
bool auth_mgr_get_api_key(char* buffer);

/// Generate a fresh API key, persist it, and copy it into @p buffer.
bool auth_mgr_regenerate_api_key(char* buffer);

bool auth_mgr_validate_api_key(const char* key);

#ifdef __cplusplus
}
#endif
