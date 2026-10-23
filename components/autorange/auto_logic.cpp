/// @file auto_logic.cpp
/// @brief See auto_logic.h.

#include "auto_logic.h"

#include <cmath>

namespace autorange {
namespace {

float snap(float v, float step) {
    if (step <= 0.0f) step = 1.0f;
    return std::round(v / step) * step;
}

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace

Range normalize(Range r, float step, Moved moved) {
    if (step <= 0.0f) step = 1.0f;
    const float gap = std::ceil(kMinGap / step - 1e-4f) * step;
    r.lo = clampf(snap(r.lo, step), kMinC, kMaxC - gap);
    r.hi = clampf(snap(r.hi, step), kMinC + gap, kMaxC);
    if (r.hi - r.lo < gap - 1e-4f) {
        if (moved == Moved::Lo) r.hi = r.lo + gap;
        else r.lo = r.hi - gap;
    }
    return r;
}

Dir initial(float room, const Range& r) {
    return room < (r.lo + r.hi) / 2.0f ? Dir::Heat : Dir::Cool;
}

Dir decide(Dir cur, float room, const Range& r, float margin) {
    if (cur == Dir::Heat && room >= r.hi + margin - 1e-4f) return Dir::Cool;
    if (cur == Dir::Cool && room <= r.lo - margin + 1e-4f) return Dir::Heat;
    return cur;
}

Range from_single(float t, float step) {
    Range r{t - kMinGap / 2.0f, t + kMinGap / 2.0f};
    return normalize(r, step, t < (kMinC + kMaxC) / 2.0f ? Moved::Lo : Moved::Hi);
}

float midpoint(const Range& r, float step) {
    return clampf(snap((r.lo + r.hi) / 2.0f, step), kMinC, kMaxC);
}

Range shift_active(const Range& r, Dir d, float t, float step) {
    Range n = r;
    if (d == Dir::Heat) {
        n.lo = t;
        return normalize(n, step, Moved::Lo);
    }
    n.hi = t;
    return normalize(n, step, Moved::Hi);
}

const char* dir_str(Dir d) { return d == Dir::Heat ? "HEAT" : "COOL"; }

}  // namespace autorange
