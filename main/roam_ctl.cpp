/// @file roam_ctl.cpp
/// @brief Drives roam::Policy from the live Wi-Fi link (see roam_ctl.h).

#include "roam_ctl.h"
#include "roam_logic.h"
#include "events.h"
#include "wifi_manager.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "esp_log.h"
#include "esp_roaming.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace roam_ctl {

static const char* TAG = "roam";

namespace {

constexpr int kPeriodMs   = 5000;
constexpr int kMaxScanAps = 16;

// Directed-roam handoff stage, shared between the roam task and the event task.
enum Stage : uint8_t { kIdle, kLeaving, kJoining };
std::atomic<uint8_t> s_stage{kIdle};
std::atomic<bool> s_pinned{false};  // our BSSID/channel pin may be in the STA config
roam::Action s_target{};            // written by the roam task before kLeaving
TaskHandle_t s_task = nullptr;

int64_t now_s() { return esp_timer_get_time() / 1000000; }

roam::Bssid to_bssid(const uint8_t* b) {
    roam::Bssid r;
    std::memcpy(r.data(), b, r.size());
    return r;
}

std::string fmt(const roam::Bssid& b) {
    char s[18];
    std::snprintf(s, sizeof(s), "%02x:%02x:%02x:%02x:%02x:%02x",
                  b[0], b[1], b[2], b[3], b[4], b[5]);
    return s;
}

// Drop our pin; the flag only clears once the config is confirmed clear, so a
// failed attempt is retried by the next event / recovery.
void clear_pin() {
    if (!s_pinned.load()) return;
    wifi_config_t cfg = {};
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) != ESP_OK) return;
    if (cfg.sta.bssid_set || cfg.sta.channel) {
        cfg.sta.bssid_set = 0;
        cfg.sta.channel = 0;
        esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "clearing BSSID pin failed: %s", esp_err_to_name(err));
            return;
        }
    }
    s_pinned = false;
}

// Back to normal operation: no pin, the roaming app owns reconnects again, and
// reconnect now if we're not associated.
void recover() {
    s_stage = kIdle;
    clear_pin();
    roam_enable_reconnect();
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) esp_wifi_connect();
}

// Directed roam (like the IDF app's legacy roam): with the app's auto-reconnect
// paused, disconnect; the DISCONNECTED event then pins the target BSSID/channel
// and connects (on_sta_disconnected). The pin is dropped once connected; a
// failed join or the policy's timeout (Abort) recovers via recover().
void roam_to(const roam::Action& a) {
    s_target = a;
    roam_disable_reconnect();
    vTaskDelay(pdMS_TO_TICKS(100));  // let the app's eloop apply it first
    s_stage = kLeaving;
    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "roam to %s: disconnect failed: %s", fmt(a.target).c_str(), esp_err_to_name(err));
        recover();
    }
}

void join_target() {
    wifi_config_t cfg = {};
    esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &cfg);
    if (err == ESP_OK) {
        cfg.sta.bssid_set = 1;
        std::memcpy(cfg.sta.bssid, s_target.target.data(), s_target.target.size());
        cfg.sta.channel = static_cast<uint8_t>(s_target.channel);
        s_pinned = true;
        err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    }
    if (err == ESP_OK) err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "roam to %s: connect failed: %s", fmt(s_target.target).c_str(), esp_err_to_name(err));
        recover();
    }
}

bool scan(roam::Policy& p, int64_t t) {
    wifi_scan_config_t sc = {};
    char ssid[33];
    strlcpy(ssid, wifi::get_ssid(), sizeof(ssid));
    sc.ssid = reinterpret_cast<uint8_t*>(ssid);
    esp_err_t err = esp_wifi_scan_start(&sc, true);  // default timing (BT coexistence)
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
        return false;
    }
    auto recs = std::make_unique<wifi_ap_record_t[]>(kMaxScanAps);
    uint16_t n = kMaxScanAps;
    if (esp_wifi_scan_get_ap_records(&n, recs.get()) != ESP_OK) return false;
    std::vector<roam::ScanAp> aps;
    aps.reserve(n);
    for (uint16_t i = 0; i < n; i++) {
        aps.push_back({to_bssid(recs[i].bssid), recs[i].rssi, recs[i].primary});
        ESP_LOGI(TAG, "scan: %s ch%d %d dBm", fmt(aps.back().bssid).c_str(),
                 aps.back().channel, aps.back().rssi);
    }
    p.scan_result(t, aps);
    return true;
}

void log_report(const roam::Report& r) {
    using K = roam::Report::Kind;
    char msg[160];
    const std::string from = fmt(r.from), to = fmt(r.to);
    switch (r.kind) {
    case K::Roaming:
        std::snprintf(msg, sizeof(msg), "Wi-Fi roaming from %s (%d dBm) to %s (%d dBm)",
                      from.c_str(), r.from_rssi, to.c_str(), r.to_rssi);
        break;
    case K::Roamed:
        std::snprintf(msg, sizeof(msg), "Wi-Fi roamed to %s", to.c_str());
        break;
    case K::Moved:
        std::snprintf(msg, sizeof(msg), "Wi-Fi moved from %s to %s", from.c_str(), to.c_str());
        break;
    case K::Kept:
        std::snprintf(msg, sizeof(msg), "Wi-Fi staying on %s (%d dBm, was %d dBm on %s)",
                      to.c_str(), r.to_rssi, r.from_rssi, from.c_str());
        break;
    case K::RoamBack:
        std::snprintf(msg, sizeof(msg),
                      "Wi-Fi %s is weaker (%d dBm vs %d dBm) — returning to %s and avoiding it for an hour",
                      from.c_str(), r.from_rssi, r.to_rssi, to.c_str());
        break;
    case K::RolledBack:
        std::snprintf(msg, sizeof(msg), "Wi-Fi back on %s", to.c_str());
        break;
    case K::RoamFailed:
        std::snprintf(msg, sizeof(msg), "Wi-Fi roam to %s failed — avoiding it for an hour", to.c_str());
        break;
    default:
        return;
    }
    ESP_LOGI(TAG, "%s", msg);
    events::log(events::Cat::Net, events::Actor::System, msg);
}

void task(void*) {
    roam::Policy policy;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(kPeriodMs));
        const int64_t t = now_s();
        wifi_ap_record_t ap = {};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            policy.sample(t, to_bssid(ap.bssid), ap.rssi, ap.primary);
        } else {
            policy.disconnected(t);
        }
        roam::Action a = policy.tick(t);
        if (a.act == roam::Act::Scan) {
            if (scan(policy, t)) {
                a = policy.tick(t);  // a confirmed roam goes out now, not 5 s later
            } else {
                policy.scan_failed(t);
            }
        }
        switch (a.act) {
        case roam::Act::Roam:
            roam_to(a);
            break;
        case roam::Act::Abort:
            recover();
            break;
        case roam::Act::Scan:  // only after a scan; picked up next period
        case roam::Act::None:
            break;
        }
        for (const auto& r : policy.take_reports()) log_report(r);
    }
}

}  // namespace

void disable_app_triggers() {
    struct roam_config cfg = {};
    esp_err_t err = roam_get_config_params(&cfg);
    if (err == ESP_OK) {
        cfg.low_rssi_roam_trigger = false;
        cfg.scan_monitor = false;
        err = roam_set_config_params(&cfg);
    }
    if (err != ESP_OK) ESP_LOGW(TAG, "disabling roaming-app triggers failed: %s", esp_err_to_name(err));
}

void start() {
    if (s_task) return;
    xTaskCreate(task, "roam", 4096, nullptr, 2, &s_task);
}

void on_sta_connected() {
    s_stage = kIdle;
    clear_pin();
}

bool on_sta_disconnected() {
    switch (s_stage.load()) {
    case kLeaving:
        s_stage = kJoining;
        join_target();
        return true;
    case kJoining:
        ESP_LOGW(TAG, "roam to %s: join failed", fmt(s_target.target).c_str());
        recover();
        return false;
    default:
        clear_pin();
        return false;
    }
}

}  // namespace roam_ctl
