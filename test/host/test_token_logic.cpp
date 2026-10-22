// Host-side unit tests for integration tokens (token_logic.cpp).
//
// Build & run (see .github/workflows/host-tests.yml):
//   g++ -std=c++17 -Wall -Wextra -I components/tokens/include
//       components/tokens/token_logic.cpp test/host/test_token_logic.cpp

#include "token_logic.h"

#include <cstdio>
#include <string>

static int g_failures = 0;

#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);       \
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

using namespace tokens;

namespace {

const std::string kId = "0a1b2c3d";
const std::string kSecret = "00112233445566778899aabbccddeeff";

Entry entry(const std::string& id, Scope scope = Scope::Control, uint8_t fill = 1) {
    Entry e;
    e.id = id;
    e.label = "Home Assistant";
    e.scope = scope;
    e.created = 1700000000;
    e.hash.fill(fill);
    return e;
}

void test_parse_token() {
    std::string id, secret;
    CHECK(parse_token(format_token(kId, kSecret), &id, &secret));
    CHECK(id == kId && secret == kSecret);

    CHECK(!parse_token("", &id, &secret));
    CHECK(!parse_token("hlt2_" + kId + "_" + kSecret, &id, &secret));      // prefix
    CHECK(!parse_token("hlt1_" + kId + "-" + kSecret, &id, &secret));      // separator
    CHECK(!parse_token("hlt1_" + kId + "_" + kSecret + "0", &id, &secret)); // too long
    CHECK(!parse_token("hlt1_0A1B2C3D_" + kSecret, &id, &secret));         // uppercase
    CHECK(!parse_token("hlt1_" + kId + "_" + kSecret.substr(1) + "g", &id, &secret));
}

void test_scope() {
    Scope s;
    CHECK(parse_scope("read", &s) && s == Scope::Read);
    CHECK(parse_scope("control", &s) && s == Scope::Control);
    CHECK(parse_scope("admin", &s) && s == Scope::Admin);
    CHECK(!parse_scope("Admin", &s));
    CHECK(!parse_scope("", &s));
    CHECK(std::string(scope_str(Scope::Control)) == "control");
}

void test_sanitize_label() {
    CHECK(sanitize_label("  Home Assistant \n") == "Home Assistant");
    CHECK(sanitize_label("a\tb\x7f") == "ab");
    CHECK(sanitize_label("   ").empty());
    CHECK(sanitize_label(std::string(40, 'x')).size() == kLabelMax);
    // 31 ASCII bytes then "é" (2 bytes) straddles the limit: drop it whole.
    std::string s = std::string(31, 'x') + "\xc3\xa9" + "yy";
    CHECK(sanitize_label(s) == std::string(31, 'x'));
}

void test_table_add_remove_match() {
    Table t;
    CHECK(t.add(entry(kId)));
    CHECK(!t.add(entry(kId)));  // duplicate id
    Entry bad = entry("zzzzzzzz");
    CHECK(!t.add(bad));
    Entry nolabel = entry("11111111");
    nolabel.label = "";
    CHECK(!t.add(nolabel));

    Hash good;
    good.fill(1);
    Hash wrong;
    wrong.fill(2);
    CHECK(t.match(kId, good) != nullptr);
    CHECK(t.match(kId, wrong) == nullptr);
    CHECK(t.match("ffffffff", good) == nullptr);

    CHECK(t.remove(kId));
    CHECK(!t.remove(kId));
    CHECK(t.match(kId, good) == nullptr);

    for (size_t i = 0; i < kMaxTokens; ++i) {
        char id[9];
        std::snprintf(id, sizeof id, "%08zx", i);
        CHECK(t.add(entry(id)));
    }
    CHECK(t.full());
    CHECK(!t.add(entry("abcdef01")));
}

void test_touch() {
    Table t;
    t.add(entry(kId));
    const uint32_t day = 86400 * 20000;
    CHECK(t.touch(kId, day + 100));    // first use: persist
    CHECK(!t.touch(kId, day + 5000));  // same day: RAM only
    CHECK(t.entries()[0].last_used == day + 5000);
    CHECK(t.touch(kId, day + 86400));  // next day: persist
    CHECK(!t.touch("ffffffff", day));
}

void test_serialize_roundtrip() {
    Table t;
    Entry a = entry(kId, Scope::Admin, 7);
    a.label = "Caf\xc3\xa9 script";
    a.last_used = 1700001234;
    t.add(a);
    t.add(entry("deadbeef", Scope::Read, 9));

    Table u;
    CHECK(u.deserialize(t.serialize()));
    CHECK(u.entries().size() == 2);
    const Entry& b = u.entries()[0];
    CHECK(b.id == a.id && b.label == a.label && b.scope == Scope::Admin);
    CHECK(b.created == a.created && b.last_used == a.last_used);
    CHECK(hash_equal(b.hash, a.hash));
    CHECK(u.entries()[1].scope == Scope::Read);

    Table empty;
    CHECK(u.deserialize(empty.serialize()) && u.entries().empty());
}

void test_deserialize_rejects_garbage() {
    Table t;
    t.add(entry(kId));
    std::vector<uint8_t> blob = t.serialize();

    Table u;
    u.add(entry("deadbeef"));
    CHECK(!u.deserialize({}));
    std::vector<uint8_t> v = blob;
    v[0] = 99;  // version
    CHECK(!u.deserialize(v));
    v = blob;
    v.pop_back();  // truncated
    CHECK(!u.deserialize(v));
    v = blob;
    v.push_back(0);  // trailing junk
    CHECK(!u.deserialize(v));
    v = blob;
    v[2 + kIdLen] = 9;  // bad scope
    CHECK(!u.deserialize(v));
    // Failures leave the table untouched.
    CHECK(u.entries().size() == 1 && u.entries()[0].id == "deadbeef");
}

}  // namespace

int main() {
    test_parse_token();
    test_scope();
    test_sanitize_label();
    test_table_add_remove_match();
    test_touch();
    test_serialize_roundtrip();
    test_deserialize_rejects_garbage();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("token_logic: all tests passed\n");
    return 0;
}
