/// @file roam_ctl.h
/// @brief Station-initiated Wi-Fi roaming (glue for components/roam).
///
/// Replaces the IDF roaming app's own low-RSSI / periodic-scan triggers, which
/// roam on a single momentary reading. A low-priority task samples the link,
/// lets roam::Policy decide, runs the scans it asks for and performs directed
/// roams. The IDF app stays in charge of reconnects and 802.11v (AP-steered)
/// transitions.

#pragma once

namespace roam_ctl {

/// Turn off the IDF roaming app's triggers. Call right after esp_wifi_start()
/// (the app is (re)initialized by it, resetting its config to the Kconfig defaults).
void disable_app_triggers();

/// Start the policy task. Call once the STA is connected.
void start();

/// WIFI_EVENT_STA_CONNECTED: drop any BSSID pin left by a directed roam.
void on_sta_connected();

/// WIFI_EVENT_STA_DISCONNECTED (not a driver roam). Returns true when this is
/// the deliberate first half of a directed roam (not an outage). Otherwise drops
/// any BSSID pin so the reconnect may use whichever AP is best.
bool on_sta_disconnected();

}  // namespace roam_ctl
