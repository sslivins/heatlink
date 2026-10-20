/// @file group_config.h
/// @brief Persistent identity + membership for the shared-compressor group
/// coordination feature (Phase 1). Stores the group_id, human label, the shared
/// group_secret used to HMAC-sign peer requests, and the enrolled-peer uid list
/// in NVS, and exposes the crypto primitives (secure random id/code/secret
/// generation and HMAC-SHA256) that the pure helpers in group_proto.h cannot
/// provide on their own. A head with an empty group_id is "standalone" and pays
/// no coordination cost.
#pragma once

#include <string>
#include <vector>

#include "esp_err.h"

#include "group_proto.h"

namespace hvac_group {

/// Everything that defines this head's participation in a compressor group.
struct GroupConfig {
    std::string group_id;      ///< 32 lowercase hex chars, or "" when standalone.
    std::string group_label;   ///< display-only, e.g. "Upstairs system".
    std::string group_secret;  ///< 64 lowercase hex chars (256-bit), or "".
    std::vector<std::string> peers;  ///< enrolled sibling device_uids (not self).
};

/// Load persisted group config from NVS and cache it. @p self_uid is this
/// device's immutable id (from wifi::device_uid()); it is never added to the
/// peer list and is used to bind signatures. Safe to call once at boot.
esp_err_t init(const std::string& self_uid);

/// This device's immutable uid (as passed to init()).
std::string self_uid();

/// Cached copy of the current config (thread-safe).
GroupConfig get();

/// True when this head is enrolled in a group (valid group_id AND secret).
bool in_group();

/// Persist @p cfg to NVS and update the cache. Rejects (ESP_ERR_INVALID_ARG) a
/// config whose non-empty group_id/secret are malformed, or whose peer list
/// contains an invalid uid or this device's own uid.
esp_err_t save(const GroupConfig& cfg);

/// Update only the display label (must already be in a group). Routed through
/// the replicated LWW register so the new label propagates to every peer.
esp_err_t set_label(const std::string& label);

/// The group's replicated time-slice config, encoded "e,min,max,settle"
/// (see slice::encode_config), or "" if never set / not grouped (#133).
std::string slice_config();

/// Set the replicated time-slice config (must be in a group). Propagates to
/// every member through the replica's LWW register.
esp_err_t set_slice_config(const std::string& encoded);

/// The outdoor unit's shared off-at-setpoint config (park::encode_shared), or
/// "" if never set / not grouped. When set it overrides each head's own
/// enable / minimum-off / probe settings.
std::string park_config();

/// Set the shared off-at-setpoint config (must be in a group). Propagates to
/// every member through the replica's LWW register.
esp_err_t set_park_config(const std::string& encoded);

/// Leave the group: clears group_id/secret/label/peers and persists. Idempotent.
esp_err_t leave_group();

// ── Replicated group state (Phase 6: eventual consistency via gossip) ──────
// The label, per-head display names, and membership are a small set of CRDTs
// (group_proto.h) reconciled over the signed peer-poll. These accessors bridge
// that pure core to NVS (JSON blob) and to the gossip transport in web_ui.

/// Serialize this head's replicated group state (label + member names +
/// membership + tombstones) to a compact JSON string for the signed snapshot.
/// Returns "" when standalone.
std::string replica_json();

/// Merge a peer's replicated-state JSON (from its polled snapshot) into ours.
/// Reprojects peers/label and persists on any change. If the merge reveals this
/// head has been evicted (self removed by an admin elsewhere), it leaves the
/// group. Returns true if our state changed.
bool merge_remote_json(const std::string& json);

/// Display name currently recorded for @p uid in the replica ("" if none).
std::string member_display_name(const std::string& uid);

/// Record this head's own current display name into the replica so peers learn
/// it. No-op if unchanged or standalone. Call at boot/join and on every device
/// rename. This is the sole writer of a head's name: a head is named only by
/// its own device display name, so there is no separate admin-rename authority.
void note_self_name(const std::string& name);

/// Admin: remove a member from the group (OR-Set remove → tombstone). The
/// evicted head learns of it on its next poll and drops the group. Rejects an
/// attempt to remove this head itself (use leave_group()).
esp_err_t remove_member(const std::string& uid);

// ── Crypto primitives (hardware RNG + mbedTLS) ─────────────────────────────

/// 128-bit cryptographically-random group_id as 32 lowercase hex chars.
std::string generate_group_id();

/// 256-bit cryptographically-random group_secret as 64 lowercase hex chars.
std::string generate_group_secret();

/// HMAC-SHA256 of @p msg under a raw key.
bool hmac_sha256(const uint8_t* key, size_t key_len, const std::string& msg,
                 uint8_t out[32]);

/// 32-byte subkey HMAC(group_secret, @p label). False when standalone.
bool group_subkey(const std::string& label, uint8_t out[32]);

/// AES-256-GCM: @p out_hex = hex(iv[12] || ciphertext || tag[16]).
bool seal(const uint8_t key[32], const std::string& plain, std::string& out_hex);

/// Inverse of seal(). False if the input is malformed or fails authentication.
bool open_sealed(const uint8_t key[32], const std::string& sealed_hex,
                 std::string& plain);

/// HMAC-SHA256 of @p message under the current group_secret, lowercase hex.
/// Returns "" if the head has no valid secret.
std::string hmac_hex(const std::string& message);

/// Display-only outdoor-unit fingerprint for the House view: the first 8 hex
/// chars of SHA-256("heatlink-house:" + group_id). Advertised over mDNS so heads
/// in different groups can be sectioned by outdoor unit without revealing the
/// group_id. Never trusted for any control decision. "" when standalone.
std::string group_fingerprint();

/// Constant-time comparison of an expected HMAC hex against a provided one.
bool hmac_verify(const std::string& message, const std::string& provided_hex);

/// Generate a fresh 16-hex-char (64-bit) nonce for signing a peer request.
std::string generate_nonce();

/// True if @p uid is currently an enrolled peer of this head.
bool is_peer(const std::string& uid);

// ── Resolution ordering (Phase 3) ──────────────────────────────────────────
// A monotonic op_id, persisted in NVS, that totally orders resolution ops so a
// rebooting/late peer can reject a stale or replayed op. The counter is
// per-head and survives leave/rejoin (never decreases) so an old op_id can
// never be replayed after a group is torn down and re-formed.

/// The highest op_id this head has issued or accepted (0 if none yet).
uint64_t last_op_id();

/// Coordinator side: allocate the next op_id (bumps + persists) and return it.
/// The returned value is strictly greater than every previously issued/accepted
/// op_id on this head.
uint64_t issue_op_id();

/// Receiver side: accept @p incoming only if it is strictly greater than the
/// last seen op_id. On acceptance the counter advances and is persisted and the
/// function returns true; a stale/replayed (≤ last) op_id returns false and
/// changes nothing.
bool accept_op_id(uint64_t incoming);

// ── Joining (password-authenticated) ───────────────────────────────────────

/// What a joining head needs to adopt the group.
struct AdmitOutcome {
    std::string group_id;
    std::string group_label;
    std::string group_secret;
    std::vector<std::string> members;  ///< this head + existing peers (not joiner)
    std::string replica_json;          ///< replicated state, joiner already enrolled
    bool formed = false;               ///< this head was standalone and formed the group
};

/// Group side of a join, called once the joiner has proven it knows the admin
/// password. Forms a group first if this head is standalone (labelled
/// @p form_label), then enrolls @p joiner_uid (seeding @p joiner_name, attributed
/// to the joiner) and persists. ESP_ERR_INVALID_ARG for a bad/self uid.
esp_err_t admit_joiner(const std::string& joiner_uid, const std::string& joiner_name,
                       const std::string& form_label, AdmitOutcome& out);

/// Joiner side: adopt a group from a successful join response. Validates all
/// fields, stores group_id/label/secret, and sets the peer list to @p members
/// minus this device's own uid. When @p replica_json is non-empty and parses,
/// the joiner adopts the group's exact replicated state (versions, names,
/// tombstones) so membership/labels are immediately consistent; otherwise it
/// synthesizes a fresh replica from @p members and @p label. Returns
/// ESP_ERR_INVALID_ARG on malformed input.
esp_err_t join_group(const std::string& group_id, const std::string& label,
                     const std::string& secret,
                     const std::vector<std::string>& members,
                     const std::string& replica_json = "");

}  // namespace hvac_group
