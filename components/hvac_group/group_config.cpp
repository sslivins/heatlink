/// @file group_config.cpp
/// @brief NVS persistence + crypto primitives for the group coordination
/// feature. The deterministic string/format logic lives in group_proto.cpp
/// (host-tested); this file adds the hardware RNG, mbedTLS HMAC, and NVS I/O.

#include "group_config.h"

#include <cstring>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "psa/crypto.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "group_proto.h"

namespace hvac_group {

namespace {
const char* TAG = "hvgroup";
constexpr char kNvsNs[] = "hvgroup";

SemaphoreHandle_t s_mtx = nullptr;
GroupConfig       s_cfg;
GroupReplica      s_replica;  // authoritative for label + membership (Phase 6)
std::string       s_self_uid;
uint64_t          s_op_id = 0;  // monotonic resolution op counter (Phase 3)

// Keep a group label printable and bounded before it hits NVS.
std::string sanitize_label(const std::string& in) {
    std::string out;
    for (char ch : in) {
        if (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7f) continue;
        out.push_back(ch);
        if (out.size() >= 48) break;
    }
    return out;
}

struct Lock {
    Lock()  { if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY); }
    ~Lock() { if (s_mtx) xSemaphoreGive(s_mtx); }
};

bool nvs_get_str_std(nvs_handle_t h, const char* key, std::string& out) {
    size_t len = 0;
    if (nvs_get_str(h, key, nullptr, &len) != ESP_OK || len == 0) return false;
    std::string buf(len, '\0');
    if (nvs_get_str(h, key, buf.data(), &len) != ESP_OK) return false;
    if (!buf.empty() && buf.back() == '\0') buf.pop_back();  // drop NUL terminator
    out = buf;
    return true;
}

std::string random_hex(size_t nbytes) {
    std::vector<uint8_t> buf(nbytes);
    esp_fill_random(buf.data(), buf.size());
    return to_hex(buf.data(), buf.size());
}

// Validate a config before persisting: non-empty id/secret must be well-formed,
// and no peer may be malformed or equal to this device's own uid.
bool config_ok(const GroupConfig& c) {
    if (!c.group_id.empty() && !is_valid_group_id(c.group_id)) return false;
    if (!c.group_secret.empty() && !is_valid_group_secret(c.group_secret)) return false;
    for (const auto& p : c.peers) {
        if (!is_valid_uid(p)) return false;
        if (p == s_self_uid) return false;
    }
    return true;
}

// ── Replicated-state (CRDT) JSON + projection (device side) ─────────────────
// Compact keys keep the snapshot small enough for the poller's body cap.

std::string serialize_replica_locked(const GroupReplica& r) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "l", static_cast<double>(r.lamport));
    cJSON* lbl = cJSON_AddObjectToObject(root, "lbl");
    cJSON_AddStringToObject(lbl, "v", r.label.value.c_str());
    cJSON_AddNumberToObject(lbl, "ver", static_cast<double>(r.label.version));
    cJSON_AddStringToObject(lbl, "o", r.label.origin.c_str());
    if (r.slice.version != 0) {
        cJSON* sl = cJSON_AddObjectToObject(root, "sl");
        cJSON_AddStringToObject(sl, "v", r.slice.value.c_str());
        cJSON_AddNumberToObject(sl, "ver", static_cast<double>(r.slice.version));
        cJSON_AddStringToObject(sl, "o", r.slice.origin.c_str());
    }
    if (r.park.version != 0) {
        cJSON* pk = cJSON_AddObjectToObject(root, "pk");
        cJSON_AddStringToObject(pk, "v", r.park.value.c_str());
        cJSON_AddNumberToObject(pk, "ver", static_cast<double>(r.park.version));
        cJSON_AddStringToObject(pk, "o", r.park.origin.c_str());
    }
    cJSON* ms = cJSON_AddArrayToObject(root, "m");
    for (const auto& m : r.members) {
        cJSON* mo = cJSON_CreateObject();
        cJSON_AddStringToObject(mo, "u", m.uid.c_str());
        cJSON_AddStringToObject(mo, "nv", m.name.value.c_str());
        cJSON_AddNumberToObject(mo, "nver", static_cast<double>(m.name.version));
        cJSON_AddStringToObject(mo, "no", m.name.origin.c_str());
        cJSON_AddNumberToObject(mo, "a", static_cast<double>(m.add_version));
        cJSON_AddItemToArray(ms, mo);
    }
    cJSON* ts = cJSON_AddArrayToObject(root, "t");
    for (const auto& t : r.tombstones) {
        cJSON* to = cJSON_CreateObject();
        cJSON_AddStringToObject(to, "u", t.first.c_str());
        cJSON_AddNumberToObject(to, "v", static_cast<double>(t.second));
        cJSON_AddItemToArray(ts, to);
    }
    char* s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    std::string out = s ? s : "";
    if (s) cJSON_free(s);
    return out;
}

// The encoded time-slice / shared park configs (comma-separated integers);
// decoded and sanitized by their components, so only shape is checked here.
bool is_valid_slice_value(const char* v) {
    size_t n = 0;
    for (; v[n]; ++n) {
        if (n >= 32) return false;
        if (!((v[n] >= '0' && v[n] <= '9') || v[n] == ',')) return false;
    }
    return n > 0;
}

// Read a uint64 Lamport clock from a cJSON number (bounded, never negative).
uint64_t json_u64(const cJSON* n) {
    if (!n || !cJSON_IsNumber(n) || n->valuedouble < 0) return 0;
    return static_cast<uint64_t>(n->valuedouble);
}

// Parse untrusted replica JSON, applying the same caps as merge_replica so a
// hostile peer can't blow up memory. Returns false on unparseable input.
bool parse_replica(const std::string& json, GroupReplica& out) {
    out = GroupReplica{};
    if (json.empty()) return false;
    cJSON* root = cJSON_Parse(json.c_str());
    if (!root) return false;
    out.lamport = json_u64(cJSON_GetObjectItem(root, "l"));
    if (const cJSON* lbl = cJSON_GetObjectItem(root, "lbl")) {
        const cJSON* v = cJSON_GetObjectItem(lbl, "v");
        const cJSON* o = cJSON_GetObjectItem(lbl, "o");
        out.label.value   = (v && cJSON_IsString(v)) ? sanitize_label(v->valuestring) : "";
        out.label.version = json_u64(cJSON_GetObjectItem(lbl, "ver"));
        out.label.origin  = (o && cJSON_IsString(o) && is_valid_uid(o->valuestring))
                                ? o->valuestring : "";
    }
    if (const cJSON* sl = cJSON_GetObjectItem(root, "sl")) {
        const cJSON* v = cJSON_GetObjectItem(sl, "v");
        const cJSON* o = cJSON_GetObjectItem(sl, "o");
        if (v && cJSON_IsString(v) && is_valid_slice_value(v->valuestring)) {
            out.slice.value   = v->valuestring;
            out.slice.version = json_u64(cJSON_GetObjectItem(sl, "ver"));
            out.slice.origin  = (o && cJSON_IsString(o) && is_valid_uid(o->valuestring))
                                    ? o->valuestring : "";
        }
    }
    if (const cJSON* pk = cJSON_GetObjectItem(root, "pk")) {
        const cJSON* v = cJSON_GetObjectItem(pk, "v");
        const cJSON* o = cJSON_GetObjectItem(pk, "o");
        if (v && cJSON_IsString(v) && is_valid_slice_value(v->valuestring)) {
            out.park.value   = v->valuestring;
            out.park.version = json_u64(cJSON_GetObjectItem(pk, "ver"));
            out.park.origin  = (o && cJSON_IsString(o) && is_valid_uid(o->valuestring))
                                   ? o->valuestring : "";
        }
    }
    if (const cJSON* ms = cJSON_GetObjectItem(root, "m"); ms && cJSON_IsArray(ms)) {
        const cJSON* mo = nullptr;
        cJSON_ArrayForEach(mo, ms) {
            if (out.members.size() >= kMaxReplicaMembers) break;
            const cJSON* u = cJSON_GetObjectItem(mo, "u");
            if (!u || !cJSON_IsString(u) || !is_valid_uid(u->valuestring)) continue;
            ReplicaMember m;
            m.uid = u->valuestring;
            const cJSON* nv = cJSON_GetObjectItem(mo, "nv");
            const cJSON* no = cJSON_GetObjectItem(mo, "no");
            m.name.value   = (nv && cJSON_IsString(nv)) ? sanitize_label(nv->valuestring) : "";
            m.name.version = json_u64(cJSON_GetObjectItem(mo, "nver"));
            m.name.origin  = (no && cJSON_IsString(no) && is_valid_uid(no->valuestring))
                                 ? no->valuestring : "";
            m.add_version  = json_u64(cJSON_GetObjectItem(mo, "a"));
            out.members.push_back(m);
        }
    }
    if (const cJSON* ts = cJSON_GetObjectItem(root, "t"); ts && cJSON_IsArray(ts)) {
        const cJSON* to = nullptr;
        cJSON_ArrayForEach(to, ts) {
            if (out.tombstones.size() >= kMaxReplicaTombstones) break;
            const cJSON* u = cJSON_GetObjectItem(to, "u");
            if (!u || !cJSON_IsString(u) || !is_valid_uid(u->valuestring)) continue;
            out.tombstones.emplace_back(u->valuestring, json_u64(cJSON_GetObjectItem(to, "v")));
        }
    }
    cJSON_Delete(root);
    return true;
}

// Project the authoritative replica onto the legacy GroupConfig fields that the
// rest of the firmware reads (group panel label, poll loop, is_peer).
void reproject_locked() {
    s_cfg.group_label = sanitize_label(s_replica.label.value);
    s_cfg.peers.clear();
    for (const auto& uid : replica_peer_uids(s_replica, s_self_uid))
        if (is_valid_uid(uid)) s_cfg.peers.push_back(uid);
}

// Seed a fresh replica from legacy (pre-CRDT) config or a plain member list —
// used on upgrade and by join fallback. Self is always a member.
void seed_replica_locked(const std::string& label,
                         const std::vector<std::string>& peers) {
    s_replica = GroupReplica{};
    replica_add_member(s_replica, s_self_uid);
    for (const auto& p : peers)
        if (is_valid_uid(p) && p != s_self_uid) replica_add_member(s_replica, p);
    if (!label.empty()) replica_set_label(s_replica, sanitize_label(label), s_self_uid);
}

// True if this head is still a present member of its own replica.
bool self_is_member_locked() {
    for (const auto& m : s_replica.members)
        if (m.uid == s_self_uid) return true;
    return false;
}

esp_err_t persist_locked(const GroupConfig& c) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNvsNs, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_str(h, "gid", c.group_id.c_str());
    nvs_set_str(h, "label", c.group_label.c_str());
    nvs_set_str(h, "secret", c.group_secret.c_str());
    nvs_set_str(h, "peers", serialize_peers(c.peers).c_str());
    nvs_set_str(h, "replica", serialize_replica_locked(s_replica).c_str());
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// Persist just the monotonic op_id (kept separate from the group blob so it
// survives a leave_group() and never rolls backward).
esp_err_t persist_op_id_locked(uint64_t v) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNvsNs, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_u64(h, "op_id", v);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}
}  // namespace

esp_err_t init(const std::string& uid) {
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    Lock lk;
    s_self_uid = uid;
    GroupConfig c;
    nvs_handle_t h;
    std::string replica_blob;
    if (nvs_open(kNvsNs, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_str_std(h, "gid", c.group_id);
        nvs_get_str_std(h, "label", c.group_label);
        nvs_get_str_std(h, "secret", c.group_secret);
        std::string peers_blob;
        if (nvs_get_str_std(h, "peers", peers_blob))
            c.peers = deserialize_peers(peers_blob);
        nvs_get_str_std(h, "replica", replica_blob);
        nvs_get_u64(h, "op_id", &s_op_id);  // absent → stays 0
        nvs_close(h);
    }
    // Defend against corrupt NVS: a malformed id/secret means "not grouped".
    if (!c.group_id.empty() && !is_valid_group_id(c.group_id)) {
        ESP_LOGW(TAG, "ignoring malformed stored group_id");
        c = GroupConfig{};
        replica_blob.clear();
    }
    // Drop any self/invalid peer that somehow got stored.
    std::vector<std::string> clean;
    for (const auto& p : c.peers)
        if (is_valid_uid(p) && p != s_self_uid) clean.push_back(p);
    c.peers = clean;

    // Establish the authoritative replica. Prefer the persisted CRDT blob; on a
    // firmware upgrade (no blob yet) synthesize one from the legacy fields so
    // membership/label carry forward, then persist it for next boot.
    bool need_persist = false;
    if (c.group_id.empty()) {
        s_replica = GroupReplica{};
    } else if (parse_replica(replica_blob, s_replica) && self_is_member_locked()) {
        // adopted persisted replica
    } else {
        seed_replica_locked(c.group_label, c.peers);
        need_persist = true;
    }
    s_cfg = c;
    if (!c.group_id.empty()) reproject_locked();  // keep peers/label consistent with replica
    if (need_persist) persist_locked(s_cfg);
    ESP_LOGI(TAG, "loaded: %s (%u peer(s))",
             s_cfg.group_id.empty() ? "standalone" : s_cfg.group_id.c_str(),
             static_cast<unsigned>(s_cfg.peers.size()));
    return ESP_OK;
}

std::string self_uid() { Lock lk; return s_self_uid; }

GroupConfig get() { Lock lk; return s_cfg; }

bool in_group() {
    Lock lk;
    return is_valid_group_id(s_cfg.group_id) && is_valid_group_secret(s_cfg.group_secret);
}

esp_err_t save(const GroupConfig& cfg) {
    Lock lk;
    if (!config_ok(cfg)) return ESP_ERR_INVALID_ARG;
    // Rebuild the authoritative replica from the supplied config so the CRDT
    // stays the single source of truth for label + membership.
    if (cfg.group_id.empty()) s_replica = GroupReplica{};
    else                      seed_replica_locked(cfg.group_label, cfg.peers);
    GroupConfig c = cfg;
    if (!cfg.group_id.empty()) {
        c.group_label = sanitize_label(cfg.group_label);
        c.peers       = replica_peer_uids(s_replica, s_self_uid);
    }
    esp_err_t err = persist_locked(c);
    if (err == ESP_OK) s_cfg = c;
    return err;
}

esp_err_t set_label(const std::string& label) {
    Lock lk;
    if (s_cfg.group_id.empty()) return ESP_ERR_INVALID_STATE;
    replica_set_label(s_replica, sanitize_label(label), s_self_uid);
    reproject_locked();
    return persist_locked(s_cfg);
}

std::string slice_config() {
    Lock lk;
    if (s_cfg.group_id.empty()) return "";
    return s_replica.slice.value;
}

esp_err_t set_slice_config(const std::string& encoded) {
    Lock lk;
    if (s_cfg.group_id.empty()) return ESP_ERR_INVALID_STATE;
    if (!is_valid_slice_value(encoded.c_str())) return ESP_ERR_INVALID_ARG;
    if (!replica_set_slice(s_replica, encoded, s_self_uid)) return ESP_OK;
    return persist_locked(s_cfg);
}

std::string park_config() {
    Lock lk;
    if (s_cfg.group_id.empty()) return "";
    return s_replica.park.value;
}

esp_err_t set_park_config(const std::string& encoded) {
    Lock lk;
    if (s_cfg.group_id.empty()) return ESP_ERR_INVALID_STATE;
    if (!is_valid_slice_value(encoded.c_str())) return ESP_ERR_INVALID_ARG;
    if (!replica_set_park(s_replica, encoded, s_self_uid)) return ESP_OK;
    return persist_locked(s_cfg);
}

esp_err_t leave_group() {
    Lock lk;
    s_replica = GroupReplica{};
    GroupConfig empty;
    esp_err_t err = persist_locked(empty);
    if (err == ESP_OK) s_cfg = empty;
    return err;
}

std::string replica_json() {
    Lock lk;
    if (s_cfg.group_id.empty()) return "";
    return serialize_replica_locked(s_replica);
}

bool merge_remote_json(const std::string& json) {
    Lock lk;
    if (s_cfg.group_id.empty()) return false;
    GroupReplica remote;
    if (!parse_replica(json, remote)) return false;
    if (!merge_replica(s_replica, remote)) return false;
    // If an admin on another head evicted us, we're no longer a member — drop
    // the group entirely (mirrors leave_group) so we stop coordinating.
    if (!self_is_member_locked()) {
        s_replica = GroupReplica{};
        GroupConfig empty;
        if (persist_locked(empty) == ESP_OK) s_cfg = empty;
        ESP_LOGW(TAG, "evicted from group by peer; left");
        return true;
    }
    reproject_locked();
    persist_locked(s_cfg);
    return true;
}

std::string member_display_name(const std::string& uid) {
    Lock lk;
    return replica_member_name(s_replica, uid);
}

void note_self_name(const std::string& name) {
    Lock lk;
    if (s_cfg.group_id.empty()) return;
    if (replica_set_name(s_replica, s_self_uid, sanitize_label(name), s_self_uid)) {
        reproject_locked();
        persist_locked(s_cfg);
    }
}

esp_err_t remove_member(const std::string& uid) {
    Lock lk;
    if (s_cfg.group_id.empty()) return ESP_ERR_INVALID_STATE;
    if (uid == s_self_uid) return ESP_ERR_INVALID_ARG;  // use leave_group() instead
    if (replica_remove_member(s_replica, uid)) {
        reproject_locked();
        return persist_locked(s_cfg);
    }
    return ESP_OK;  // not a member is a no-op
}

std::string generate_group_id()     { return random_hex(kGroupIdHexLen / 2); }
std::string generate_group_secret() { return random_hex(kGroupSecretHexLen / 2); }

std::string generate_nonce() { return random_hex(8); }  // 64-bit → 16 hex chars

bool is_peer(const std::string& uid) {
    Lock lk;
    for (const auto& p : s_cfg.peers)
        if (p == uid) return true;
    return false;
}

uint64_t last_op_id() { Lock lk; return s_op_id; }

uint64_t issue_op_id() {
    Lock lk;
    ++s_op_id;
    persist_op_id_locked(s_op_id);  // best-effort; cache is authoritative
    return s_op_id;
}

bool accept_op_id(uint64_t incoming) {
    Lock lk;
    if (incoming <= s_op_id) return false;
    s_op_id = incoming;
    persist_op_id_locked(s_op_id);
    return true;
}

bool hmac_sha256(const uint8_t* key, size_t key_len, const std::string& msg,
                 uint8_t out[32]) {
    if (!key || key_len == 0 || psa_crypto_init() != PSA_SUCCESS) return false;
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attr, PSA_BYTES_TO_BITS(key_len));
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_key_id_t key_id = 0;
    if (psa_import_key(&attr, key, key_len, &key_id) != PSA_SUCCESS) return false;
    size_t out_len = 0;
    const psa_status_t st = psa_mac_compute(
        key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256),
        reinterpret_cast<const uint8_t*>(msg.data()), msg.size(), out, 32, &out_len);
    psa_destroy_key(key_id);
    return st == PSA_SUCCESS && out_len == 32;
}

namespace {
bool secret_bytes(std::vector<uint8_t>& key) {
    std::string secret_hex;
    { Lock lk; secret_hex = s_cfg.group_secret; }
    return is_valid_group_secret(secret_hex) && from_hex(secret_hex, key) && !key.empty();
}

constexpr size_t kGcmIv  = 12;
constexpr size_t kGcmTag = 16;

bool import_aes_key(const uint8_t key[32], psa_key_usage_t usage, psa_key_id_t& id) {
    if (psa_crypto_init() != PSA_SUCCESS) return false;
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 256);
    psa_set_key_usage_flags(&attr, usage);
    psa_set_key_algorithm(&attr, PSA_ALG_GCM);
    return psa_import_key(&attr, key, 32, &id) == PSA_SUCCESS;
}
}  // namespace

bool group_subkey(const std::string& label, uint8_t out[32]) {
    std::vector<uint8_t> key;
    return secret_bytes(key) && hmac_sha256(key.data(), key.size(), label, out);
}

bool seal(const uint8_t key[32], const std::string& plain, std::string& out_hex) {
    psa_key_id_t id = 0;
    if (!import_aes_key(key, PSA_KEY_USAGE_ENCRYPT, id)) return false;
    std::vector<uint8_t> buf(kGcmIv + plain.size() + kGcmTag);
    esp_fill_random(buf.data(), kGcmIv);
    size_t olen = 0;
    const psa_status_t st = psa_aead_encrypt(
        id, PSA_ALG_GCM, buf.data(), kGcmIv, nullptr, 0,
        reinterpret_cast<const uint8_t*>(plain.data()), plain.size(),
        buf.data() + kGcmIv, buf.size() - kGcmIv, &olen);
    psa_destroy_key(id);
    if (st != PSA_SUCCESS || olen != plain.size() + kGcmTag) return false;
    out_hex = to_hex(buf.data(), buf.size());
    return true;
}

bool open_sealed(const uint8_t key[32], const std::string& sealed_hex,
                 std::string& plain) {
    std::vector<uint8_t> buf;
    if (!from_hex(sealed_hex, buf) || buf.size() < kGcmIv + kGcmTag) return false;
    psa_key_id_t id = 0;
    if (!import_aes_key(key, PSA_KEY_USAGE_DECRYPT, id)) return false;
    std::string out(buf.size() - kGcmIv - kGcmTag, '\0');
    size_t olen = 0;
    const psa_status_t st = psa_aead_decrypt(
        id, PSA_ALG_GCM, buf.data(), kGcmIv, nullptr, 0, buf.data() + kGcmIv,
        buf.size() - kGcmIv, reinterpret_cast<uint8_t*>(out.data()), out.size(), &olen);
    psa_destroy_key(id);
    if (st != PSA_SUCCESS || olen != out.size()) return false;
    plain = std::move(out);
    return true;
}

std::string hmac_hex(const std::string& message) {
    std::vector<uint8_t> key;
    uint8_t out[32];
    if (!secret_bytes(key) || !hmac_sha256(key.data(), key.size(), message, out)) return "";
    return to_hex(out, sizeof(out));
}

std::string group_fingerprint() {
    std::string gid;
    { Lock lk; gid = s_cfg.group_id; }
    if (gid.empty() || psa_crypto_init() != PSA_SUCCESS) return "";
    const std::string m = "heatlink-house:" + gid;
    uint8_t out[32];
    size_t out_len = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, reinterpret_cast<const uint8_t*>(m.data()),
                         m.size(), out, sizeof(out), &out_len) != PSA_SUCCESS)
        return "";
    return to_hex(out, 4);
}

bool hmac_verify(const std::string& message, const std::string& provided_hex) {
    const std::string expected = hmac_hex(message);
    if (expected.empty() || expected.size() != provided_hex.size()) return false;
    // Constant-time compare so a timing side-channel can't leak the signature.
    uint8_t diff = 0;
    for (size_t i = 0; i < expected.size(); ++i)
        diff |= static_cast<uint8_t>(expected[i] ^ provided_hex[i]);
    return diff == 0;
}

esp_err_t admit_joiner(const std::string& joiner_uid, const std::string& joiner_name,
                       const std::string& form_label, AdmitOutcome& out) {
    Lock lk;
    if (!is_valid_uid(joiner_uid) || joiner_uid == s_self_uid) return ESP_ERR_INVALID_ARG;
    GroupReplica prev_rep = s_replica;
    GroupConfig  prev_cfg = s_cfg;
    out = AdmitOutcome{};
    if (!is_valid_group_id(s_cfg.group_id) || !is_valid_group_secret(s_cfg.group_secret)) {
        // Standalone: form a group with this head as its sole member.
        GroupConfig c;
        c.group_id     = generate_group_id();
        c.group_secret = generate_group_secret();
        seed_replica_locked(sanitize_label(form_label), {});
        s_cfg = c;
        out.formed = true;
    }
    // The joiner needs everyone already in the group (computed before adding it).
    out.members.push_back(s_self_uid);
    for (const auto& p : s_cfg.peers) out.members.push_back(p);
    replica_add_member(s_replica, joiner_uid);
    // Seed the joiner's name attributed to the joiner itself, preserving the
    // "a head is named only by itself" invariant.
    if (!joiner_name.empty())
        replica_set_name(s_replica, joiner_uid, sanitize_label(joiner_name), joiner_uid);
    reproject_locked();
    esp_err_t err = persist_locked(s_cfg);
    if (err != ESP_OK) {
        s_replica = prev_rep;
        s_cfg = prev_cfg;
        reproject_locked();
        out = AdmitOutcome{};
        return err;
    }
    out.group_id     = s_cfg.group_id;
    out.group_label  = s_cfg.group_label;
    out.group_secret = s_cfg.group_secret;
    out.replica_json = serialize_replica_locked(s_replica);
    ESP_LOGI(TAG, "enrolled peer %s%s", joiner_uid.c_str(), out.formed ? " (group formed)" : "");
    return ESP_OK;
}

esp_err_t join_group(const std::string& group_id, const std::string& label,
                     const std::string& secret,
                     const std::vector<std::string>& members,
                     const std::string& replica_json) {
    if (!is_valid_group_id(group_id) || !is_valid_group_secret(secret))
        return ESP_ERR_INVALID_ARG;

    Lock lk;
    const std::string self = s_self_uid;

    // Build the replica we'll adopt into a local first, so a validation failure
    // leaves existing state untouched.
    GroupReplica newrep;
    if (!replica_json.empty() && parse_replica(replica_json, newrep)) {
        // Prefer the owner's exact CRDT state; make sure we're in it.
        bool self_present = false;
        for (const auto& m : newrep.members)
            if (m.uid == self) { self_present = true; break; }
        if (!self_present) replica_add_member(newrep, self);
    } else {
        // Fallback: synthesize from the plain member list + label.
        for (const auto& m : members) {
            if (m == self) continue;
            if (!is_valid_uid(m)) return ESP_ERR_INVALID_ARG;
        }
        replica_add_member(newrep, self);
        for (const auto& m : members)
            if (is_valid_uid(m) && m != self) replica_add_member(newrep, m);
        if (!label.empty()) replica_set_label(newrep, sanitize_label(label), self);
    }

    s_replica       = newrep;
    s_cfg           = GroupConfig{};
    s_cfg.group_id  = group_id;
    s_cfg.group_secret = secret;
    reproject_locked();  // fills label + peers from the adopted replica
    return persist_locked(s_cfg);
}

}  // namespace hvac_group
