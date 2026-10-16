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
};

/// Load config + persisted managed flag from NVS. Call before the CN105 task.
void init(const Hooks& hooks);

/// Advance the machine; call ~1 Hz from the CN105 task.
void tick();

/// User/HA/group set desired power (replaces a direct setPowerSetting).
void set_desired_power(bool on);

/// User/HA/group changed mode or setpoint. Starts a probe if parked.
void notify_intent_change();

/// Physical settings -> what users should see (power ON while parked).
cn105::Settings overlay(cn105::Settings s);

park::Config config();
/// Sanitize, persist and apply new tunables. Returns the sanitized config.
park::Config set_config(const park::Config& c);

struct Snapshot {
    bool        enabled = false;
    park::State state = park::State::Off;
    bool        parked = false;
    std::string reason;
    int32_t     next_probe_s = -1;
    bool        unit_on = false;  ///< physical unit power at the last tick
};
Snapshot snapshot();

}  // namespace park_ctl
