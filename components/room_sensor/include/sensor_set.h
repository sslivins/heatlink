/// @file sensor_set.h
/// @brief A head's wireless room sensors (#142) — pure, host-testable.
///
/// Up to kMaxSensors sensors per head, each reported by a driver (transport):
///   - BTHome: Bluetooth adverts (Shelly BLU H&T, ATC-flashed Xiaomi, ...)
///   - ShellyRpc: Shelly Gen2+ Wi-Fi devices with a temperature component,
///     reporting through webhooks the head installs at pairing.
/// The room temperature is the average of the fresh sensors; with none fresh
/// the head falls back to its own sensor. The firmware glue
/// (main/room_sensor_ctl.cpp) feeds adverts/reports and a monotonic clock.

#pragma once

#include "bthome.h"
#include "shelly_ht.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace room_sensor {

constexpr size_t   kMaxSensors = 4;
constexpr uint32_t kBleStaleS = 600;                  ///< BTHome adverts every ~1 min
constexpr uint32_t kWifiStaleS = 2 * 3600 + 15 * 60;  ///< Shelly reports at least every 2 h
constexpr uint32_t kPairWindowS = 60;                 ///< BLE "press the button" window
constexpr uint32_t kNearbyTtlS = 120;                 ///< nearby list forgets silent devices
constexpr size_t   kMaxNearby = 16;

enum class Kind : uint8_t {
    BTHome = 0,     ///< id = BLE address "AA:BB:CC:DD:EE:FF"
    ShellyRpc = 1,  ///< id = Shelly device id, e.g. "shellyhtg3-aabbccddeeff"
};
const char* kind_str(Kind k);         ///< "bthome" / "shelly"
bool kind_from_str(const std::string& s, Kind& out);
uint32_t stale_s(Kind k);

struct Sample {
    bool     valid = false;   ///< a temperature has been received
    float    temp_c = 0.0f;
    bool     has_humidity = false;
    float    humidity = 0.0f;
    bool     has_battery = false;
    uint8_t  battery = 0;
    bool     has_rssi = false;
    int      rssi = 0;
    uint32_t temp_at_s = 0;   ///< when temp_c was received
    uint32_t seen_at_s = 0;   ///< last advert/report of any kind
    bool     seen = false;
};

struct Sensor {
    std::string id;
    Kind        kind = Kind::BTHome;
    std::string name;
    Sample      sample;
    // ShellyRpc driver state (persisted by the glue).
    std::string host;          ///< sensor IP (follows its reports)
    std::string token;         ///< secret in its webhook URL
    std::string hook_ip;       ///< head IP its webhooks point at
    float       step_c = shelly::kDefaultStepC;  ///< wanted report threshold
    bool        sync_pending = false;  ///< hooks/step not yet applied on the sensor
    std::string sync_error;    ///< last apply failure (not persisted)
};

struct Nearby {
    std::string addr;
    std::string name;
    int         rssi = 0;
    bool        encrypted = false;
    bool        has_temp = false;
    float       temp_c = 0.0f;
    uint32_t    seen_at_s = 0;
};

class SensorSet {
public:
    enum class Result { Ignored, Updated, Paired, Full };

    const std::vector<Sensor>& sensors() const { return sensors_; }
    Sensor* find(const std::string& id);
    const Sensor* find(const std::string& id) const;
    bool full() const { return sensors_.size() >= kMaxSensors; }

    /// Add (or replace the same id). False when full. Ends the BLE window.
    bool add(const Sensor& s);
    bool remove(const std::string& id);
    void clear() { sensors_.clear(); }

    static bool fresh(const Sensor& s, uint32_t now_s) {
        return s.sample.valid && now_s - s.sample.temp_at_s < stale_s(s.kind);
    }
    /// Average of the fresh sensors' temperatures. False if none is fresh.
    bool room_temp(uint32_t now_s, float* out, size_t* n_fresh = nullptr) const;

    // ── BTHome pairing ────────────────────────────────────────────────
    /// Open the "press the button on your sensor" window (clears the nearby list).
    void start_pairing(uint32_t now_s);
    void cancel_pairing() { pairing_ = false; }
    bool pairing(uint32_t now_s) const {
        return pairing_ && now_s - pair_start_ < kPairWindowS;
    }
    uint32_t pairing_remaining_s(uint32_t now_s) const;
    /// Manual pick from the nearby list (Paired / Full / Ignored if unknown,
    /// stale or encrypted).
    Result select(const std::string& addr, uint32_t now_s);
    /// Feed one BTHome advert. In the window an unencrypted button press
    /// pairs a new sensor; adverts from paired BTHome sensors update them.
    Result on_advert(const std::string& addr, int rssi, const bthome::Reading& r,
                     uint32_t now_s);
    /// BTHome devices heard during the window, strongest first.
    std::vector<Nearby> nearby(uint32_t now_s) const;
    /// Bluetooth scanning is needed (a BTHome sensor is paired, or pairing).
    bool wants_scan(uint32_t now_s) const;

    // ── Shelly webhook reports ────────────────────────────────────────
    /// A report whose token matches a ShellyRpc sensor updates it; *id_out
    /// names it. Ignored otherwise.
    Result on_report(const shelly::Report& r, uint32_t now_s, std::string* id_out = nullptr);

private:
    void note_nearby(const std::string& addr, int rssi, const bthome::Reading& r,
                     uint32_t now_s);
    static void apply(Sample& s, const bthome::Reading& r, int rssi, uint32_t now_s);

    std::vector<Sensor> sensors_;
    bool        pairing_ = false;
    uint32_t    pair_start_ = 0;
    std::vector<Nearby> nearby_;
};

}  // namespace room_sensor
