/// @file remote_temp.h
/// @brief Remote room temperature fed by an integration (Home Assistant).
///
/// A feed is a lease: each update carries a time-to-live, and when it runs
/// out without renewal the head clears the remote temperature and goes back
/// to the unit's own sensor, so an integration that stops (HA down, sensor
/// unavailable) can't leave the unit regulating on a stale reading. Paired
/// room sensors (#142) take precedence: updates are refused while they own
/// the remote temperature, and a lease they take over is dropped.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace remote_temp {

constexpr uint32_t kDefaultTtlS = 600;
constexpr uint32_t kMinTtlS = 60;
constexpr uint32_t kMaxTtlS = 3600;
constexpr float kMinC = 1.0f;
constexpr float kMaxC = 40.0f;

struct Hooks {
    /// Send the remote temperature to the unit (<= 0 clears it).
    std::function<void(float celsius)> set_remote;
    /// True while paired room sensors own the remote temperature.
    std::function<bool()> sensor_active;
    /// The feed started, was cleared or expired: @p msg for the event log;
    /// @p source is the feed's ("api" / "mqtt"); @p who is the actor, empty
    /// when the head itself ended the feed.
    std::function<void(const std::string& msg, const std::string& source,
                       const std::string& who)>
        on_event;
};

void init(const Hooks& hooks);

enum class Res { Ok, SensorOwns, OutOfRange };

/// Set (or renew) the feed. @p ttl_s 0 means no expiry (legacy MQTT and
/// /api/settings); otherwise it is clamped to [kMinTtlS, kMaxTtlS].
Res set(float celsius, uint32_t ttl_s, const std::string& source, const std::string& who);

/// Stop the feed and return the unit to its own sensor. No-op when idle.
void clear(const std::string& who);

struct Snapshot {
    bool active = false;
    float temp = 0;
    std::string source;     ///< "api" / "mqtt"
    std::string who;        ///< actor (token name, user)
    uint32_t age_s = 0;     ///< since the last update
    int32_t expires_in_s = -1;  ///< -1: no expiry
};
Snapshot snapshot();

}  // namespace remote_temp
