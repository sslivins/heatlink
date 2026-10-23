/// @file auto_logic.h
/// @brief HeatLink-managed Auto: a "heat to" / "cool to" setpoint range.
///
/// Used only while a head runs under Tandem. The unit itself always runs plain
/// HEAT or COOL; this logic picks which one from trusted room readings
/// (park::Machine::sample) and holds the matching setpoint. The range's gap
/// plus park's restart threshold give the hysteresis, so there is no minimum
/// time between switches. Pure, host-testable.

#pragma once

namespace autorange {

constexpr float kMinGap = 2.0f;   ///< cool_to - heat_to, at least
constexpr float kMinC = 16.0f;    ///< setpoint limits the unit accepts
constexpr float kMaxC = 31.0f;

enum class Dir { Heat, Cool };

struct Range {
    float lo = 20.0f;  ///< heat to
    float hi = 24.0f;  ///< cool to
};

/// Which end the caller just set: that end wins, the other moves to keep the gap.
enum class Moved { Lo, Hi };

/// Round to @p step (0.5 or 1), clamp to the unit's limits and keep the gap
/// (at least kMinGap, rounded up to the step).
Range normalize(Range r, float step, Moved moved);

/// Direction to start in: heat below the midpoint, else cool.
Dir initial(float room, const Range& r);

/// After a trusted reading of @p room: the direction to run. Heat flips to
/// cool once the room reaches cool_to + @p margin; cool flips to heat at
/// heat_to - @p margin (margin = park's restart threshold).
Dir decide(Dir cur, float room, const Range& r, float margin);

/// The setpoint the unit runs in @p d.
inline float active(const Range& r, Dir d) { return d == Dir::Heat ? r.lo : r.hi; }

/// The range for a single setpoint (native AUTO, IR remote): t-1 .. t+1.
Range from_single(float t, float step);

/// The single setpoint for a range (back to native AUTO): its midpoint.
float midpoint(const Range& r, float step);

/// A setpoint command while in Auto moves the active end.
Range shift_active(const Range& r, Dir d, float t, float step);

const char* dir_str(Dir d);  ///< "HEAT" / "COOL"

}  // namespace autorange
