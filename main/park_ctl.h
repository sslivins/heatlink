/// @file park_ctl.h
/// @brief Firmware glue for the "off at setpoint" feature (park::Machine).
///
/// Owns the state machine behind a mutex, persists its config and the
/// "managed" (parked/probing) flag in NVS, ticks it from the CN105 task and
/// executes the power Actions it returns. While a head is parked its desired
/// power is ON, so overlay() reports power ON to the web UI, HA and group peers.

#pragma once

#include "park_logic.h"
#include "cn105_types.h"

#include <cstdint>
#include <functional>
#include <string>

namespace park_ctl {

struct Hooks {
    std::function<park::Inputs()> read;            ///< snapshot of the unit
    std::function<void(bool on)>  set_power;       ///< physical power command
    std::function<void()>         on_change;       ///< republish state (MQTT)
    std::function<std::string()>  get_fan;         ///< unit's reported fan speed
    std::function<void(const std::string&)> set_fan;  ///< physical fan command
    std::function<std::string()>  get_mode;        ///< unit's reported mode ("" if not connected)
    std::function<void(const std::string&)> set_mode; ///< physical mode command
};

/// Air checks run the fan at its lowest speed (QUIET) and restore the user's
/// setting afterwards. A user fan command during a check becomes the setting
/// to restore (returns true: don't send it now); otherwise returns false.
bool request_fan(const std::string& fan);

/// A held Tandem air check runs the unit in FAN mode and restores the user's
/// mode afterwards. A mode command during one becomes the mode to restore
/// (returns true: don't send it now); otherwise returns false.
bool request_mode(const std::string& mode);

/// Load config + persisted managed flag from NVS. Call before the CN105 task.
void init(const Hooks& hooks);

/// Advance the machine; call ~1 Hz from the CN105 task.
void tick();

/// User/HA/group set desired power (replaces a direct setPowerSetting).
void set_desired_power(bool on);

/// User/HA/group changed mode or setpoint. Starts a probe if parked.
void notify_intent_change();

/// Physical settings -> what users should see (power ON while parked, the
/// user's fan speed / mode during an air check).
cn105::Settings overlay(cn105::Settings s);

/// True when the enable / min-off / probe settings come from the group's
/// shared outdoor-unit config rather than this head alone.
bool shared();

/// The configured settings (what /api/park shows: this head's own, with the
/// outdoor unit's shared fields applied), regardless of slicing.
park::Config config();
/// Sanitize, persist and apply new tunables. Returns the sanitized config.
park::Config set_config(const park::Config& c);

/// Time-slicing (#133): while @p active the machine runs even if the user
/// disabled off-at-setpoint (it is how heads park and report satisfaction),
/// and @p hold keeps the unit parked off until its slice comes up. @p check
/// (held only) asks for an air check of the room if there is no fresh verdict.
void set_slice(bool active, bool hold, bool check = false);

struct Snapshot {
    bool        enabled = false;
    park::State state = park::State::Off;
    bool        parked = false;
    std::string reason;
    int32_t     next_probe_s = -1;
    bool        unit_on = false;  ///< physical unit power at the last tick
    bool        satisfied = false;  ///< parked at setpoint (not held for a slice)
    bool        held = false;       ///< held off for another slice (#133)
    bool        forced = false;     ///< running only because slicing is on
    bool        slicing = false;    ///< group time-slicing is enabled
    bool        quiet_fan = false;  ///< air check running with the fan at QUIET
    park::Need  need = park::Need::Yes;  ///< held: does the room need its slice
    bool        checking = false;   ///< held air check running (unit in FAN mode)
    park::Machine::Sample sample;   ///< latest trusted room reading (managed Auto)
};
Snapshot snapshot();

}  // namespace park_ctl
