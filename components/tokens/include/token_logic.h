// Integration tokens: per-client bearer tokens for scripts and Home Assistant.
//
// A token is "hlt1_<id>_<secret>": an 8-hex-digit public id and a 32-hex-digit
// (128-bit) secret. The head stores only SHA-256(secret) next to the id, a
// label, a scope and timestamps, so tokens can be listed and revoked one at a
// time. Pure logic (no ESP-IDF): hashing and storage are done by the caller.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tokens {

constexpr size_t kIdLen = 8;       ///< hex digits
constexpr size_t kSecretLen = 32;  ///< hex digits (128 bits)
constexpr size_t kLabelMax = 32;   ///< bytes
constexpr size_t kMaxTokens = 8;
constexpr const char* kPrefix = "hlt1_";

/// What a token may do. Each level includes the ones below it.
enum class Scope : uint8_t {
    Read = 1,     ///< read-only (GET requests)
    Control = 2,  ///< also change the climate, like the climate-only login
    Admin = 3,    ///< full admin access
};

const char* scope_str(Scope s);
/// "read" / "control" / "admin"; false for anything else.
bool parse_scope(const std::string& s, Scope* out);

using Hash = std::array<uint8_t, 32>;

struct Entry {
    std::string id;     ///< kIdLen lowercase hex
    std::string label;  ///< sanitized, 1..kLabelMax bytes
    Scope scope = Scope::Read;
    uint32_t created = 0;    ///< unix seconds (0 = clock not set)
    uint32_t last_used = 0;  ///< unix seconds (0 = never)
    Hash hash{};             ///< SHA-256 of the secret's hex text
};

/// Split "hlt1_<id>_<secret>" into its parts. False unless both are
/// lowercase hex of the right length.
bool parse_token(const std::string& token, std::string* id, std::string* secret);
/// Build the token text from an id and secret.
std::string format_token(const std::string& id, const std::string& secret);
/// Lowercase hex of @p n bytes.
std::string to_hex(const uint8_t* data, size_t n);

/// Trim, drop control characters and cut to kLabelMax bytes (on a UTF-8
/// boundary). Empty input yields "".
std::string sanitize_label(const std::string& label);

/// Constant-time comparison.
bool hash_equal(const Hash& a, const Hash& b);

class Table {
public:
    const std::vector<Entry>& entries() const { return entries_; }
    bool full() const { return entries_.size() >= kMaxTokens; }
    bool has_id(const std::string& id) const;
    /// Add an entry; false if full, the id is taken or the entry is invalid.
    bool add(const Entry& e);
    /// Remove by id; false if absent.
    bool remove(const std::string& id);
    /// Remove every entry labelled @p label whose scope is at most @p max, so
    /// re-pairing an integration replaces its token instead of piling up
    /// copies. Returns how many were removed.
    size_t remove_label(const std::string& label, Scope max);
    void clear() { entries_.clear(); }
    /// The entry whose id matches and whose hash equals @p hash, or nullptr.
    const Entry* match(const std::string& id, const Hash& hash) const;
    /// Record use at @p now. Returns true when the stored value should be
    /// persisted (first use, or a new UTC day) to bound flash writes.
    bool touch(const std::string& id, uint32_t now);

    /// Versioned binary encoding for NVS.
    std::vector<uint8_t> serialize() const;
    /// Replace the table from serialize() output; false (table unchanged) on
    /// malformed input.
    bool deserialize(const std::vector<uint8_t>& blob);

private:
    std::vector<Entry> entries_;
};

}  // namespace tokens
