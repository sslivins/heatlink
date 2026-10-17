/// @file room_sensor_ctl.h
/// @brief Firmware glue for wireless room sensors (#142): NimBLE passive
/// scanning for BTHome adverts (Shelly BLU H&T), the paired sensor in NVS,
/// and feeding its reading to the unit as the remote room temperature.
///
/// While the paired sensor is fresh its temperature is sent to the unit
/// (CN105 remote temperature) and used by off-at-setpoint instead of air
/// checks. If it goes quiet for room_sensor::kStaleS the remote temperature
/// is cleared and the head falls back to its own sensor.

#pragma once

#include "sensor_tracker.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace room_sensor_ctl {

struct Hooks {
    /// Send the remote room temperature to the unit (<= 0 clears it).
    std::function<void(float celsius)> set_remote;
    /// The feed started / stopped (republish state, etc.). Optional.
    std::function<void()> on_change;
};

/// Load the paired sensor from NVS and start the worker task. BLE itself
/// starts only once a sensor is paired or pairing begins.
void init(const Hooks& hooks);

/// The paired sensor's temperature while it is fresh.
bool fresh_temp(float* out);

/// The sensor currently owns the unit's remote temperature (HA's
/// remote_temp/set is ignored meanwhile).
bool active();

struct Snapshot {
    bool        paired = false;
    std::string addr;
    std::string name;
    bool        fresh = false;
    room_sensor::Sample sample;
    int32_t     temp_age_s = -1;   ///< seconds since the last temperature, -1 none
    int32_t     seen_age_s = -1;   ///< seconds since any advert, -1 none
    bool        pairing = false;
    uint32_t    pairing_remaining_s = 0;
    std::vector<room_sensor::Nearby> nearby;
    std::vector<int32_t> nearby_age_s;
    bool        ble_ready = false; ///< stack up and scanning
    std::string ble_error;         ///< non-empty if the stack failed to start
    uint32_t    ble_adverts = 0;   ///< advertisements heard since boot (any device)
};
Snapshot snapshot();

/// Open the 60 s "press the button on the sensor" window.
void start_pairing();
void cancel_pairing();
/// Pair a sensor from the nearby list. False if unknown or encrypted.
bool select(const std::string& addr);
void unpair();

}  // namespace room_sensor_ctl
