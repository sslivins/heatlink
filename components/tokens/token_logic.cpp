#include "token_logic.h"

#include <algorithm>

namespace tokens {

namespace {

constexpr uint8_t kBlobVersion = 1;

bool is_lower_hex(const std::string& s, size_t len) {
    if (s.size() != len) return false;
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

bool valid_entry(const Entry& e) {
    return is_lower_hex(e.id, kIdLen) && !e.label.empty() && e.label.size() <= kLabelMax &&
           (e.scope == Scope::Read || e.scope == Scope::Control || e.scope == Scope::Admin);
}

void put_u32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

uint32_t get_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

const char* scope_str(Scope s) {
    switch (s) {
        case Scope::Read: return "read";
        case Scope::Control: return "control";
        case Scope::Admin: return "admin";
    }
    return "read";
}

bool parse_scope(const std::string& s, Scope* out) {
    if (s == "read") *out = Scope::Read;
    else if (s == "control") *out = Scope::Control;
    else if (s == "admin") *out = Scope::Admin;
    else return false;
    return true;
}

bool parse_token(const std::string& token, std::string* id, std::string* secret) {
    const std::string prefix = kPrefix;
    if (token.size() != prefix.size() + kIdLen + 1 + kSecretLen) return false;
    if (token.compare(0, prefix.size(), prefix) != 0) return false;
    if (token[prefix.size() + kIdLen] != '_') return false;
    std::string i = token.substr(prefix.size(), kIdLen);
    std::string s = token.substr(prefix.size() + kIdLen + 1);
    if (!is_lower_hex(i, kIdLen) || !is_lower_hex(s, kSecretLen)) return false;
    *id = i;
    *secret = s;
    return true;
}

std::string format_token(const std::string& id, const std::string& secret) {
    return std::string(kPrefix) + id + "_" + secret;
}

std::string to_hex(const uint8_t* data, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s.push_back(d[data[i] >> 4]);
        s.push_back(d[data[i] & 0xf]);
    }
    return s;
}

std::string sanitize_label(const std::string& label) {
    std::string s;
    for (unsigned char c : label) {
        if (c < 0x20 || c == 0x7f) continue;
        s.push_back(static_cast<char>(c));
    }
    size_t b = s.find_first_not_of(' ');
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(' ');
    s = s.substr(b, e - b + 1);
    if (s.size() > kLabelMax) {
        size_t cut = kLabelMax;
        // Back up over UTF-8 continuation bytes so a character isn't split.
        while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
        s.resize(cut);
        size_t t = s.find_last_not_of(' ');
        s.resize(t == std::string::npos ? 0 : t + 1);
    }
    return s;
}

bool hash_equal(const Hash& a, const Hash& b) {
    uint8_t diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    return diff == 0;
}

bool Table::has_id(const std::string& id) const {
    return std::any_of(entries_.begin(), entries_.end(),
                       [&](const Entry& e) { return e.id == id; });
}

bool Table::add(const Entry& e) {
    if (full() || !valid_entry(e) || has_id(e.id)) return false;
    entries_.push_back(e);
    return true;
}

bool Table::remove(const std::string& id) {
    auto it = std::find_if(entries_.begin(), entries_.end(),
                           [&](const Entry& e) { return e.id == id; });
    if (it == entries_.end()) return false;
    entries_.erase(it);
    return true;
}

const Entry* Table::match(const std::string& id, const Hash& hash) const {
    for (const Entry& e : entries_) {
        if (e.id == id) return hash_equal(e.hash, hash) ? &e : nullptr;
    }
    return nullptr;
}

bool Table::touch(const std::string& id, uint32_t now) {
    for (Entry& e : entries_) {
        if (e.id != id) continue;
        bool persist = e.last_used == 0 || e.last_used / 86400 != now / 86400;
        e.last_used = now;
        return persist;
    }
    return false;
}

// Layout: version, count, then per entry: id[8], scope, label_len, label,
// created u32le, last_used u32le, hash[32].
std::vector<uint8_t> Table::serialize() const {
    std::vector<uint8_t> out;
    out.push_back(kBlobVersion);
    out.push_back(static_cast<uint8_t>(entries_.size()));
    for (const Entry& e : entries_) {
        out.insert(out.end(), e.id.begin(), e.id.end());
        out.push_back(static_cast<uint8_t>(e.scope));
        out.push_back(static_cast<uint8_t>(e.label.size()));
        out.insert(out.end(), e.label.begin(), e.label.end());
        put_u32(out, e.created);
        put_u32(out, e.last_used);
        out.insert(out.end(), e.hash.begin(), e.hash.end());
    }
    return out;
}

bool Table::deserialize(const std::vector<uint8_t>& blob) {
    if (blob.size() < 2 || blob[0] != kBlobVersion) return false;
    size_t n = blob[1];
    if (n > kMaxTokens) return false;
    Table t;
    size_t p = 2;
    for (size_t i = 0; i < n; ++i) {
        if (p + kIdLen + 2 > blob.size()) return false;
        Entry e;
        e.id.assign(reinterpret_cast<const char*>(&blob[p]), kIdLen);
        p += kIdLen;
        e.scope = static_cast<Scope>(blob[p++]);
        size_t ll = blob[p++];
        if (p + ll + 8 + e.hash.size() > blob.size()) return false;
        e.label.assign(reinterpret_cast<const char*>(&blob[p]), ll);
        p += ll;
        e.created = get_u32(&blob[p]);
        e.last_used = get_u32(&blob[p + 4]);
        p += 8;
        std::copy(blob.begin() + p, blob.begin() + p + e.hash.size(), e.hash.begin());
        p += e.hash.size();
        if (!t.add(e)) return false;
    }
    if (p != blob.size()) return false;
    entries_ = std::move(t.entries_);
    return true;
}

}  // namespace tokens
