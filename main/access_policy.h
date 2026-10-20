/// @file access_policy.h
/// @brief Admin-set limits on what climate-only users can do, and whether the
///        power-cut admin-password reset is allowed. Persisted in NVS.
#pragma once

namespace access_policy {

/// Setpoint range allowed for climate-only users (web UI user role / guests).
/// Admins, API keys, MQTT and Home Assistant are not limited.
struct Limits {
    bool enabled = false;
    float min_c = 16.0f;
    float max_c = 26.0f;
};

void init();

Limits limits();
/// Validates (16..31 °C, 0.5 °C steps, min <= max) and persists. False if invalid.
bool set_limits(const Limits& l);
/// Clamp a climate-only user's setpoint to the configured range (no-op when off).
float clamp_user_setpoint(float c);

/// Whether cutting power 5 times opens the admin-password reset window.
/// The Home Assistant reset button always works.
bool power_reset_enabled();
void set_power_reset_enabled(bool on);

}  // namespace access_policy
