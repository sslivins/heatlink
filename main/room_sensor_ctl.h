/// @file room_sensor_ctl.h
/// @brief Firmware glue for wireless room sensors (#142).
///
/// Up to room_sensor::kMaxSensors sensors per head, from two drivers:
///   - BTHome: NimBLE passive scan (Shelly BLU H&T, ...).
///   - Shelly RPC: Shelly Gen2+ Wi-Fi sensors; at pairing the head installs
///     webhooks that call GET /api/sensors/report on every report.
/// While any sensor is fresh, their average is sent to the unit as the
/// remote room temperature and used by off-at-setpoint instead of air
/// checks. With none fresh the remote temperature is cleared and the head
/// falls back to its own sensor.

#pragma once

#include "sensor_set.h"
#include "shelly_rpc.h"

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

/// Load the sensors from NVS and start the worker task. BLE starts only once
/// a BTHome sensor is paired or Bluetooth pairing begins.
void init(const Hooks& hooks);

/// The room temperature (average of fresh sensors), if any is fresh.
bool fresh_temp(float* out);

/// The sensors currently own the unit's remote temperature (HA's
/// remote_temp/set is ignored meanwhile).
bool active();

/// Cheap summary for status displays.
struct Summary {
    size_t n = 0;          ///< paired sensors
    size_t n_fresh = 0;
    bool   has_temp = false;
    float  temp = 0.0f;    ///< average of fresh sensors
};
Summary summary();

struct SensorView {
    room_sensor::Sensor s;
    bool     fresh = false;
    int32_t  temp_age_s = -1;  ///< -1 = no temperature yet
    int32_t  seen_age_s = -1;
    uint32_t stale_s = 0;
};

struct Snapshot {
    std::vector<SensorView> sensors;
    bool   has_temp = false;     ///< room temperature available (any fresh)
    float  temp = 0.0f;
    size_t n_fresh = 0;
    // Bluetooth pairing window.
    bool     ble_pairing = false;
    uint32_t ble_remaining_s = 0;
    std::vector<room_sensor::Nearby> nearby;
    std::vector<int32_t> nearby_age_s;
    // Wi-Fi pairing window.
    bool     wifi_pairing = false;
    uint32_t wifi_remaining_s = 0;
    std::vector<shelly_rpc::Found> found;
    std::string pair_state = "idle";  ///< idle / working / ok / error
    std::string pair_message;
    // Bluetooth stack.
    bool        ble_scanning = false;
    std::string ble_error;
    uint32_t    ble_adverts = 0;
};
Snapshot snapshot();

enum class Res { Ok, Full, NotFound, Invalid, Busy };

/// Open the 60 s "press the button on the sensor" Bluetooth window.
Res start_ble_pairing();
/// Look for awake Shelly sensors on the network for 3 minutes.
Res start_wifi_pairing();
void cancel_pairing();
/// Pair a BTHome sensor from the nearby list.
Res select(const std::string& addr);
/// Pair the Shelly at host (IP or name); runs in the background, result in
/// Snapshot::pair_state / pair_message.
Res pair_wifi(const std::string& host);
Res remove(const std::string& id);
/// Rename (name != nullptr) and/or set a Shelly report step (step_c > 0).
Res configure(const std::string& id, const std::string* name, float step_c);

/// A webhook call: query string and the caller's IP. False if the token
/// doesn't match any sensor.
bool on_report(const std::string& query, const std::string& peer_ip);

}  // namespace room_sensor_ctl
