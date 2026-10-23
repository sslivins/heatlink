// Host-side unit tests for managed Auto's range logic (auto_logic.cpp).
//
// Build & run (see .github/workflows/host-tests.yml):
//   g++ -std=c++17 -Wall -Wextra -I components/autorange/include
//       components/autorange/auto_logic.cpp test/host/test_auto_logic.cpp

#include "auto_logic.h"

#include <cstdio>

static int g_failures = 0;

#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);       \
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

using namespace autorange;

static bool eq(float a, float b) { return a > b - 1e-3f && a < b + 1e-3f; }

static void test_normalize_gap() {
    // Raising heat-to into the gap pushes cool-to up.
    Range r = normalize({23.0f, 24.0f}, 1.0f, Moved::Lo);
    CHECK(eq(r.lo, 23.0f) && eq(r.hi, 25.0f));
    // Lowering cool-to into the gap pushes heat-to down.
    r = normalize({20.0f, 21.0f}, 1.0f, Moved::Hi);
    CHECK(eq(r.lo, 19.0f) && eq(r.hi, 21.0f));
    // A wide range is left alone.
    r = normalize({19.0f, 26.0f}, 1.0f, Moved::Lo);
    CHECK(eq(r.lo, 19.0f) && eq(r.hi, 26.0f));
}

static void test_normalize_step_and_limits() {
    // Whole-degree units: rounding happens before the gap is enforced.
    Range r = normalize({20.4f, 21.6f}, 1.0f, Moved::Lo);
    CHECK(eq(r.lo, 20.0f) && eq(r.hi, 22.0f));
    // Half-degree units keep halves.
    r = normalize({20.5f, 22.5f}, 0.5f, Moved::Lo);
    CHECK(eq(r.lo, 20.5f) && eq(r.hi, 22.5f));
    // Limits: cool-to can't pass 31, so heat-to gives way.
    r = normalize({30.0f, 31.0f}, 1.0f, Moved::Lo);
    CHECK(eq(r.lo, 29.0f) && eq(r.hi, 31.0f));
    r = normalize({10.0f, 12.0f}, 1.0f, Moved::Hi);
    CHECK(eq(r.lo, 16.0f) && eq(r.hi, 18.0f));
}

static void test_initial() {
    Range r{20.0f, 24.0f};
    CHECK(initial(18.0f, r) == Dir::Heat);
    CHECK(initial(21.9f, r) == Dir::Heat);
    CHECK(initial(22.0f, r) == Dir::Cool);
    CHECK(initial(26.0f, r) == Dir::Cool);
}

static void test_decide_hysteresis() {
    Range r{20.0f, 24.0f};
    // Heating: stays heating anywhere below cool-to + margin.
    CHECK(decide(Dir::Heat, 19.0f, r, 1.0f) == Dir::Heat);
    CHECK(decide(Dir::Heat, 24.5f, r, 1.0f) == Dir::Heat);
    CHECK(decide(Dir::Heat, 25.0f, r, 1.0f) == Dir::Cool);
    // Cooling: stays cooling anywhere above heat-to - margin.
    CHECK(decide(Dir::Cool, 26.0f, r, 1.0f) == Dir::Cool);
    CHECK(decide(Dir::Cool, 19.5f, r, 1.0f) == Dir::Cool);
    CHECK(decide(Dir::Cool, 19.0f, r, 1.0f) == Dir::Heat);
}

static void test_single_and_midpoint() {
    Range r = from_single(22.0f, 1.0f);
    CHECK(eq(r.lo, 21.0f) && eq(r.hi, 23.0f));
    CHECK(eq(midpoint(r, 1.0f), 22.0f));
    // Near the top limit the range keeps the gap within limits.
    r = from_single(31.0f, 1.0f);
    CHECK(eq(r.lo, 29.0f) && eq(r.hi, 31.0f));
    r = from_single(16.0f, 1.0f);
    CHECK(eq(r.lo, 16.0f) && eq(r.hi, 18.0f));
    // Midpoint of an odd range rounds to the unit's step.
    CHECK(eq(midpoint({20.0f, 23.0f}, 0.5f), 21.5f));
}

static void test_shift_active() {
    Range r{20.0f, 24.0f};
    Range n = shift_active(r, Dir::Heat, 21.0f, 1.0f);
    CHECK(eq(n.lo, 21.0f) && eq(n.hi, 24.0f));
    n = shift_active(r, Dir::Heat, 23.0f, 1.0f);
    CHECK(eq(n.lo, 23.0f) && eq(n.hi, 25.0f));
    n = shift_active(r, Dir::Cool, 21.0f, 1.0f);
    CHECK(eq(n.lo, 19.0f) && eq(n.hi, 21.0f));
    CHECK(eq(active(n, Dir::Cool), 21.0f));
}

int main() {
    test_normalize_gap();
    test_normalize_step_and_limits();
    test_initial();
    test_decide_hysteresis();
    test_single_and_midpoint();
    test_shift_active();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("All auto_logic tests passed\n");
    return 0;
}
