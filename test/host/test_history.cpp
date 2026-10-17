// Host-side unit tests for the history log's pure logic (history_logic.cpp).
//
// Build & run (see .github/workflows/host-tests.yml):
//   g++ -std=c++17 -Wall -Wextra -I components/history/include
//       components/history/history_logic.cpp test/host/test_history.cpp

#include "history_logic.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0;

#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);       \
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

using namespace history;

namespace {

Record rec(uint32_t t, int16_t room, uint8_t flags = kFlagConnected | kFlagPower,
           uint8_t freq = 0) {
    Record r;
    r.t = t;
    r.room = room;
    r.target = 210;
    r.flags = flags;
    r.freq = freq;
    r.mode = kModeHeat;
    r.fan = 0;
    return r;
}

std::vector<Record> run(uint32_t bucket, const std::vector<Record>& in) {
    std::vector<Record> out;
    Bucketer b(bucket);
    Record o;
    for (const auto& r : in)
        if (b.add(r, o)) out.push_back(o);
    if (b.finish(o)) out.push_back(o);
    return out;
}

void test_codes() {
    CHECK(mode_code("HEAT") == kModeHeat);
    CHECK(mode_code("COOL") == kModeCool);
    CHECK(mode_code("DRY") == kModeDry);
    CHECK(mode_code("FAN") == kModeFan);
    CHECK(mode_code("AUTO") == kModeAuto);
    CHECK(mode_code("") == kModeNone);
    CHECK(fan_code("AUTO") == 0);
    CHECK(fan_code("QUIET") == 1);
    CHECK(fan_code("1") == 2);
    CHECK(fan_code("4") == 5);
    CHECK(fan_code("5") == kNoFan);
    CHECK(to_deci(21.5f) == 215);
    CHECK(to_deci(-3.25f) == -33 || to_deci(-3.25f) == -32);
    CHECK(to_deci(std::nanf("")) == kNoTemp);
}

void test_windows() {
    CHECK(clamp_window(0) == 86400);
    CHECK(clamp_window(60) == kMinWindowS);
    CHECK(clamp_window(100 * 86400) == kMaxWindowS);
    CHECK(bucket_for(4 * 3600) == 30);
    CHECK(bucket_for(86400) == 60);
    CHECK(bucket_for(7 * 86400) == 420);
    CHECK(bucket_for(14 * 86400) == 840);
    // Never more than kMaxPoints rows.
    for (uint32_t w = kMinWindowS; w <= kMaxWindowS; w += 3571)
        CHECK(w / bucket_for(w) <= kMaxPoints);
}

void test_passthrough_at_interval() {
    auto out = run(30, {rec(1000020, 200), rec(1000050, 205), rec(1000080, 210)});
    CHECK(out.size() == 3);
    CHECK(out[0].t == 1000020 && out[0].room == 200);
    CHECK(out[2].t == 1000080 && out[2].room == 210);
    CHECK(out[1].target == 210 && out[1].mode == kModeHeat && out[1].fan == 0);
}

void test_averaging_and_flags() {
    // Two 60 s buckets: [0,60) and [60,120) relative to a 60-aligned base.
    const uint32_t b = 1000020;  // multiple of 60
    Record a = rec(b, 200, kFlagConnected | kFlagPower | kFlagOperating, 40);
    Record c = rec(b + 30, 211, kFlagConnected | kFlagPower, 20);
    c.outdoor = 50;
    Record d = rec(b + 60, 220, kFlagConnected | kFlagDefrost);
    Record e = rec(b + 90, 230, kFlagConnected);
    e.mode = kModeCool;
    auto out = run(60, {a, c, d, e});
    CHECK(out.size() == 2);
    CHECK(out[0].t == b);
    CHECK(out[0].room == 206);        // (200 + 211) / 2 = 205.5 → 206
    CHECK(out[0].outdoor == 50);      // only one reading
    CHECK(out[0].sensor == kNoTemp);  // none
    CHECK(out[0].freq == 30);
    CHECK(out[0].flags & kFlagOperating);  // half the samples → set
    CHECK(out[0].flags & kFlagPower);
    CHECK(out[1].t == b + 60);
    CHECK(out[1].flags & kFlagDefrost);    // any sample → set
    CHECK(!(out[1].flags & kFlagPower));
    CHECK(out[1].mode == kModeCool);       // last sample
}

void test_minority_flag_dropped() {
    const uint32_t b = 1000020 - 1000020 % 120;
    auto out = run(120, {rec(b, 200, kFlagOperating), rec(b + 30, 200),
                         rec(b + 60, 200), rec(b + 90, 200)});
    CHECK(out.size() == 1);
    CHECK(!(out[0].flags & kFlagOperating));
}

void test_negative_rounding() {
    const uint32_t b = 1000020;
    auto out = run(60, {rec(b, -10), rec(b + 30, -15)});
    CHECK(out.size() == 1 && out[0].room == -13);  // -12.5 → -13
}

void test_backward_clock_dropped() {
    const uint32_t b = 1000020;
    auto out = run(30, {rec(b + 60, 200), rec(b, 100), rec(b + 90, 210)});
    CHECK(out.size() == 2);
    CHECK(out[0].t == b + 60 && out[1].t == b + 90);
}

void test_empty() {
    Bucketer bk(60);
    Record o;
    CHECK(!bk.finish(o));
}

void test_format_row() {
    Record r = rec(1700000010, 215);
    r.outdoor = -35;
    r.freq = 14;
    r.flags = kFlagConnected | kFlagPower | kFlagOperating;
    char buf[96];
    format_row(r, buf, sizeof(buf));
    CHECK(std::string(buf) == "[1700000010,215,210,-35,null,14,1,7,0]");
    Record n;
    n.t = 5;
    format_row(n, buf, sizeof(buf));
    CHECK(std::string(buf) == "[5,null,null,null,null,0,0,0,null]");
    // Worst case fits the handler's row buffer.
    Record w;
    w.t = 4294967295u;
    w.room = w.target = w.outdoor = w.sensor = -32767;
    w.freq = w.mode = w.flags = 255;
    w.fan = 5;
    CHECK(format_row(w, buf, sizeof(buf)) < 96);
}

}  // namespace

int main() {
    test_codes();
    test_windows();
    test_passthrough_at_interval();
    test_averaging_and_flags();
    test_minority_flag_dropped();
    test_negative_rounding();
    test_backward_clock_dropped();
    test_empty();
    test_format_row();
    if (g_failures) {
        std::printf("%d history test(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("All history tests passed\n");
    return 0;
}
