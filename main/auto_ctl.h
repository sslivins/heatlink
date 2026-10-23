/// @file auto_ctl.h
/// @brief HeatLink-managed Auto (a heat-to / cool-to range) for heads in Tandem.
///
/// Outside Tandem, AUTO is the unit's own Auto and this module stays out of
/// the way. In Tandem the unit never runs native AUTO: it runs HEAT at
/// "heat to" or COOL at "cool to", and this module picks which from trusted
/// room readings (park_ctl::Snapshot::sample). Everything below the user-facing
/// API (slicing, peers, history) sees only the physical HEAT/COOL.
///
/// - Turning Tandem on with the unit in native AUTO (or pressing AUTO on the
///   IR remote) adopts it as a range around its setpoint (t-1 .. t+1).
/// - Turning Tandem off while managed returns the unit to native AUTO at the
///   range's midpoint.
/// - Any explicit non-AUTO mode (app, HA, IR remote) leaves Auto. Power off
///   keeps it. A setpoint command moves the active end of the range.

#pragma once

#include "auto_logic.h"
#include "cn105_types.h"

#include <functional>
#include <string>

namespace auto_ctl {

struct Hooks {
    std::function<cn105::Settings()> settings;   ///< desired (park overlay)
    std::function<cn105::Status()>   status;
    std::function<bool()>            connected;  ///< CN105 link up
    std::function<float()>           step;       ///< unit's setpoint step (0.5 / 1)
    /// Apply a physical mode + setpoint as one intent (holds the slice across
    /// both writes); @p power_on also turns the head on.
    std::function<void(const std::string& mode, float temp, bool power_on)> apply;
    /// Managed state, range or direction changed (republish what users see).
    std::function<void()> on_change;
};

/// Load persisted state. Call before the CN105 task.
void init(const Hooks& hooks);

/// Advance ~1 Hz from the CN105 task, after park_ctl::tick() and before
/// slice_ctl::tick().
void tick();

/// Managed Auto is on (only ever while Tandem is on).
bool active();

struct View {
    bool             on = false;
    autorange::Range range;
    autorange::Dir   dir = autorange::Dir::Heat;
};
View view();

/// The settings users see: mode "AUTO" while managed (the physical mode is
/// in view().dir). Leaves everything else as is.
cn105::Settings present(cn105::Settings s);

/// A user/HA asked for AUTO while Tandem is on. Enters managed Auto with the
/// stored range, applies it and powers the head on.
void enter();

/// A user/HA chose a non-AUTO mode: leave managed Auto (no unit command).
void leave();

/// A setpoint command while managed: moves the active end and applies it.
void set_active(float t);

/// Set the range (from the app / HA); applied at once if managed Auto is on,
/// else stored for the next enter(). Returns active().
bool set_range(float heat_to, float cool_to);

}  // namespace auto_ctl
