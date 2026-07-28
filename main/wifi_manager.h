/// @file wifi_manager.h
/// @brief WiFi station bring-up with a SoftAP provisioning fallback.
///
/// Adapted from the arctic-sniffer WiFi manager. Credentials are read from NVS
/// (namespace "wifi", keys "ssid"/"pass"); if absent, the CONFIG_WIFI_*
/// fallback is used; if that is also empty or STA fails, the device starts a
/// SoftAP so the user can provision it.

#pragma once

#include <string>
#include <cstdint>

#include "esp_err.h"

namespace wifi {

enum class Mode { IDLE, CONNECTING, CONNECTED, PROVISIONING };

/// Short, stable per-chip id (4 hex = low 2 bytes of the factory base MAC).
/// Espressif assigns every chip a globally-unique MAC, so this is collision-free
/// for a handful of units. Reused for the SoftAP name, the mDNS hostname, and the
/// default MQTT friendly_name so all three stay unique and consistent.
std::string device_uid();

/// mDNS hostname actually in use: "<CONFIG_MDNS_HOSTNAME>-<device_uid>".
std::string mdns_hostname();

/// User-settable display name shown in the web UI, login screen and browser
/// tab. Empty when unset (callers fall back to the hostname / a default). This
/// is purely cosmetic — it does NOT affect the mDNS hostname, SoftAP name, or
/// MQTT friendly_name, so renaming never disturbs Home Assistant discovery.
std::string device_display_name();

/// Persist the display name to NVS and update the in-memory cache (no reboot).
/// Pass "" to clear it back to the default. Returns ESP_OK on success.
esp_err_t set_display_name(const char* name);

/// Web-UI temperature display unit. false = °C (default), true = °F. This is a
/// pure display/input preference for the device web UI: all firmware, MQTT and
/// Home Assistant setpoint/room values remain in °C. The head is only ever
/// commanded in °C, so the UI converts and snaps to a supported 0.5 °C step.
bool temp_unit_fahrenheit();

/// Persist the temperature display unit to NVS and update the cache (no reboot).
esp_err_t set_temp_unit(bool fahrenheit);

/// Web-UI language preference as a short code (e.g. "en", "fr"). Empty string
/// means "auto" — the browser chooses from navigator.language. A pure display
/// choice for the device web UI; firmware, MQTT and Home Assistant are unaffected.
std::string ui_language();

/// Persist the web-UI language code to NVS and update the cache (no reboot). The
/// value is sanitised to [A-Za-z0-9-] and capped; "" is valid and means "auto".
esp_err_t set_ui_language(const char* lang);

/// WiFi transmit-power cap currently in effect, in whole dBm. Lowering it shrinks
/// the radio's transmit-current peaks (which sag the current-limited CN105 5 V
/// rail and can trip brownout on buffer-less heads) at the cost of link margin.
int get_tx_power_dbm();
/// Inclusive bounds the UI/API should clamp a TX-power request to (dBm).
int tx_power_min_dbm();
int tx_power_max_dbm();

/// Persist a new TX-power cap (clamped to [tx_power_min_dbm, tx_power_max_dbm])
/// and apply it to the live radio immediately — no reboot. Returns ESP_OK on a
/// successful NVS write (the value is applied only after it is persisted).
esp_err_t set_tx_power_dbm(int dbm);

/// Bring up WiFi. Returns ESP_OK once STA is connected with an IP, or
/// ESP_ERR_NOT_FINISHED when it has fallen back to SoftAP provisioning.
esp_err_t init();

Mode get_mode();
bool is_connected();
const char* get_ip();
const char* get_ap_name();

/// Live RSSI (dBm) of the current association, or 0 when not connected.
int get_rssi();

/// Auth mode of the current association as a wifi_auth_mode_t int
/// (0 == WIFI_AUTH_OPEN), or -1 when not connected.
int get_auth();

/// SSID the device is configured to join (empty in out-of-box / provisioning).
const char* get_ssid();

/// Number of WiFi outages (an established association was lost) since boot.
/// Counted once per outage — not per reconnect retry — and only after the STA
/// has actually obtained an IP, so initial connect attempts don't count. This
/// is a session counter kept in RAM: it starts at 0 every boot, so a firmware
/// update / device reset can never inflate it.
uint32_t disconnect_count();

/// Zero the session WiFi-drop counter.
void reset_disconnect_count();

/// Whether a WiFi password is currently stored (the password itself is never
/// exposed over the API — used server-side only to preserve it on partial saves).
bool has_password();
const char* get_password();

/// Store credentials in NVS (used by the provisioning portal). Caller should
/// reboot afterwards.
esp_err_t save_credentials(const char* ssid, const char* pass);

/// Blocking scan (~2-4s) for nearby networks. Fills @p out with a JSON array
/// string: [{"ssid":..,"rssi":..,"auth":..}] deduped by SSID (strongest kept),
/// hidden/empty SSIDs dropped, sorted by RSSI descending.
esp_err_t scan_json(std::string& out);

/// Erase stored credentials so the next boot re-enters provisioning.
esp_err_t erase_credentials();

/// While an "add a zone" pairing window is open, advertise pair=1 in the
/// _mmhvac._tcp TXT (plus the human-readable group label and this head's display
/// name) so an ungrouped head can discover the session without typing an address.
/// Call with active=false to clear it — the change is announced over mDNS so
/// browsers stop offering it promptly. The pairing code is never advertised.
void set_pairing_advert(bool active, const std::string& glabel = "",
                        const std::string& name = "");

/// Browse the LAN for heads currently advertising pair=1 and return a JSON array
/// string: [{"uid":..,"name":..,"glabel":..,"host":"<ip>"}], excluding self.
esp_err_t discover_pairing(std::string& out_json);

}  // namespace wifi
