/// @file shelly_ht.h
/// @brief Shelly Wi-Fi H&T (Gen3 / Plus) helpers (#142) — pure, host-testable.
///
/// Battery Shelly H&Ts sleep and only connect to Wi-Fi to report, so they
/// can't be polled. At pairing the head installs webhooks on the sensor
/// (temperature.change + humidity.change) that call GET /api/sensors/report
/// with the readings filled in by Shelly's URL token replacement.

#pragma once

#include <string>

namespace shelly {

constexpr float kDefaultStepC = 0.5f;  ///< Shelly's default report threshold
constexpr float kMinStepC = 0.1f;
constexpr float kMaxStepC = 5.0f;
constexpr const char* kHookName = "heatlink";
constexpr const char* kReportPath = "/api/sensors/report";

/// The webhook URL the sensor calls on every report.
std::string report_url(const std::string& head_ip, const std::string& token);

struct Report {
    std::string token;
    bool  has_temp = false;
    float temp_c = 0.0f;
    bool  has_humidity = false;
    float humidity = 0.0f;
    bool  has_battery = false;
    int   battery = 0;
    bool  has_rssi = false;
    int   rssi = 0;
};

/// Parse the report query string ("k=..&t=21.5&h=40&b=80&r=-60"). Values the
/// sensor couldn't fill in (null, or the token copied verbatim) are left
/// absent. Returns false without a token.
bool parse_report(const std::string& query, Report& out);

/// Clamp a report step to [kMinStepC, kMaxStepC], rounded to 0.1 °C.
float clamp_step(float c);

/// mDNS host/instance name of a Shelly H&T ("ShellyHTG3-…", "shellyplusht-…").
bool is_ht_name(const std::string& name);

/// Length-checked, timing-independent comparison.
bool token_equal(const std::string& a, const std::string& b);

}  // namespace shelly
