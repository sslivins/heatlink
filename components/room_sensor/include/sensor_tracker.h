/// @file sensor_tracker.h
/// @brief Room-sensor pairing + freshness tracking (#142) — pure,
/// host-testable. The firmware glue (main/room_sensor.cpp) feeds it BLE
/// advertisements and a monotonic clock.

#pragma once

#include "bthome.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace room_sensor {

constexpr uint32_t kStaleS = 600;       ///< no temperature this long -> fall back
constexpr uint32_t kPairWindowS = 60;   ///< "press the button" window
constexpr uint32_t kNearbyTtlS = 120;   ///< nearby list forgets silent devices
constexpr size_t   kMaxNearby = 16;

struct Sample {
    bool     valid = false;   ///< a temperature has been received
    float    temp_c = 0.0f;
    bool     has_humidity = false;
    float    humidity = 0.0f;
    bool     has_battery = false;
    uint8_t  battery = 0;
    int      rssi = 0;
    uint32_t temp_at_s = 0;   ///< when temp_c was received
    uint32_t seen_at_s = 0;   ///< last advertisement of any kind
    bool     seen = false;
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

class Tracker {
public:
    enum class Result { Ignored, Updated, Paired };

    /// Load / replace the paired sensor ("" unpairs). Clears the sample when
    /// the address changes and ends any pairing window.
    void set_paired(const std::string& addr, const std::string& name = "");
    bool paired() const { return !addr_.empty(); }
    const std::string& addr() const { return addr_; }
    const std::string& name() const { return name_; }

    /// Open the "press the button on your sensor" window (clears the nearby list).
    void start_pairing(uint32_t now_s);
    void cancel_pairing() { pairing_ = false; }
    bool pairing(uint32_t now_s) const {
        return pairing_ && now_s - pair_start_ < kPairWindowS;
    }
    uint32_t pairing_remaining_s(uint32_t now_s) const;

    /// Manual pick from the nearby list. False if it hasn't been heard
    /// recently or is encrypted (can't be decoded).
    bool select(const std::string& addr, uint32_t now_s);

    /// Feed one BTHome advertisement. During pairing a button press from an
    /// unencrypted device pairs it; adverts from the paired sensor update the
    /// sample.
    Result on_advert(const std::string& addr, int rssi, const bthome::Reading& r,
                     uint32_t now_s);

    const Sample& sample() const { return sample_; }
    /// A temperature newer than kStaleS from the paired sensor.
    bool fresh(uint32_t now_s) const {
        return paired() && sample_.valid && now_s - sample_.temp_at_s < kStaleS;
    }
    /// Recently heard BTHome devices (only collected while pairing), strongest first.
    std::vector<Nearby> nearby(uint32_t now_s) const;
    /// BLE scanning is needed (paired, or pairing).
    bool wants_scan(uint32_t now_s) const { return paired() || pairing(now_s); }

private:
    void note_nearby(const std::string& addr, int rssi, const bthome::Reading& r,
                     uint32_t now_s);

    std::string addr_;
    std::string name_;
    Sample      sample_{};
    bool        pairing_ = false;
    uint32_t    pair_start_ = 0;
    std::vector<Nearby> nearby_;
};

}  // namespace room_sensor
