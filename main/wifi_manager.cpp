/// @file wifi_manager.cpp
/// @brief WiFi STA bring-up + SoftAP provisioning fallback.

#include "wifi_manager.h"
#include "dns_server.h"
#include "group_proto.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
#include <algorithm>
#include <mutex>
#include <atomic>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "mdns.h"
#include "cJSON.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "sdkconfig.h"

// Embedded gzip'd captive-portal page (produced at configure time).
extern const uint8_t portal_html_gz_start[] asm("_binary_portal_html_gz_start");
extern const uint8_t portal_html_gz_end[]   asm("_binary_portal_html_gz_end");

namespace wifi {

static const char* TAG = "wifi";

namespace {
constexpr int kConnectedBit = BIT0;
constexpr int kFailBit      = BIT1;
constexpr char kNvsNs[]     = "wifi";

// WiFi transmit power cap (user-tunable, persisted in the "device" NVS
// namespace under key "txpow", in whole dBm). The radio defaults to the PHY
// ceiling (20 dBm), whose transmit-current peaks sag the current-limited CN105
// 5 V rail — with no LiPo/bulk buffer on the deployed heads, the deepest sags
// can trip the brownout detector. The default keeps the full 20 dBm for the best
// link margin; the cap is exposed on the Settings page so a brownout-prone unit
// can be turned down individually against its link margin. The driver takes
// 0.25 dBm units over 8..84, so we clamp to 2..20 dBm.
constexpr int8_t kDefaultTxPowerDbm = 20;
constexpr int8_t kMinTxPowerDbm     = 2;
constexpr int8_t kMaxTxPowerDbm     = 20;
int8_t s_tx_power_dbm = kDefaultTxPowerDbm;

// Apply the current TX-power cap to the radio. Must be called after
// esp_wifi_start() — the driver rejects the setting before the PHY is up.
// Best-effort: a failure just leaves the radio at its previous level, so we log
// and continue.
void apply_tx_power_cap() {
    esp_err_t err = esp_wifi_set_max_tx_power(static_cast<int8_t>(s_tx_power_dbm * 4));
    if (err != ESP_OK) ESP_LOGW(TAG, "set_max_tx_power failed: %s", esp_err_to_name(err));
}

EventGroupHandle_t s_events = nullptr;
Mode  s_mode = Mode::IDLE;
char  s_ip[16] = "0.0.0.0";
char  s_ap_name[33] = {0};
char  s_ssid[33] = {0};
char  s_pass[65] = {0};
int   s_retries = 0;
uint32_t s_wifi_drops = 0;   // outages (established assoc lost) this session
bool  s_established  = false; // true once we've obtained an IP at least once

httpd_handle_t s_portal = nullptr;
wifi_ap_record_t s_scan[20];
uint16_t s_scan_count = 0;

// User-settable display name (shown in the web UI/login/tab). Decoupled from
// the mDNS hostname and MQTT friendly_name so renaming never breaks Home
// Assistant discovery or .local addressing. Guarded by a mutex because the
// httpd handlers that read it and the POST handler that writes it may run on
// separate tasks.
constexpr char kDeviceNvsNs[] = "device";
SemaphoreHandle_t s_name_mtx = nullptr;
std::string       s_name;
// Web-UI temperature display preference: false = °C (default), true = °F. A pure
// display/input choice for the device web UI — firmware, MQTT and Home Assistant
// values are always °C. Guarded by s_name_mtx (same "device" namespace, both low
// contention).
bool              s_temp_f = false;
// Web-UI language preference (BCP-47-ish short code, e.g. "en"/"fr"). Empty =
// "auto": the browser picks from navigator.language. A pure display choice for
// the device web UI; firmware/MQTT/HA are unaffected. Guarded by s_name_mtx.
std::string       s_ui_lang;

void load_name() {
    char buf[64] = {0};
    nvs_handle_t h;
    if (nvs_open(kDeviceNvsNs, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(buf);
        if (nvs_get_str(h, "name", buf, &len) == ESP_OK) s_name = buf;
        uint8_t tf = 0;
        if (nvs_get_u8(h, "tunit", &tf) == ESP_OK) s_temp_f = (tf != 0);
        int8_t tx = 0;
        if (nvs_get_i8(h, "txpow", &tx) == ESP_OK) {
            if (tx < kMinTxPowerDbm) tx = kMinTxPowerDbm;
            if (tx > kMaxTxPowerDbm) tx = kMaxTxPowerDbm;
            s_tx_power_dbm = tx;
        }
        char lang[16] = {0};
        size_t ll = sizeof(lang);
        if (nvs_get_str(h, "lang", lang, &ll) == ESP_OK) s_ui_lang = lang;
        nvs_close(h);
    }
}

// Trim, strip control characters, and cap to 32 bytes without splitting a
// UTF-8 sequence. Centralised here so every setter (main UI + captive portal)
// stores a consistently clean value.
std::string sanitize_name(const char* in) {
    std::string out;
    for (const char* p = in ? in : ""; *p; ++p) {
        unsigned char c = static_cast<unsigned char>(*p);
        if (c < 0x20 || c == 0x7f) continue;  // drop control chars/newlines
        out.push_back(*p);
    }
    size_t a = out.find_first_not_of(' ');
    if (a == std::string::npos) return "";
    size_t b = out.find_last_not_of(' ');
    out = out.substr(a, b - a + 1);
    constexpr size_t kMax = 32;
    if (out.size() > kMax) {
        size_t cut = kMax;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) cut--;
        out.resize(cut);
    }
    return out;
}

bool load_creds(char* ssid, size_t ssid_len, char* pass, size_t pass_len) {
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READONLY, &h) == ESP_OK) {
        size_t sl = ssid_len, pl = pass_len;
        esp_err_t e1 = nvs_get_str(h, "ssid", ssid, &sl);
        esp_err_t e2 = nvs_get_str(h, "pass", pass, &pl);
        nvs_close(h);
        if (e1 == ESP_OK && e2 == ESP_OK && ssid[0] != '\0') return true;
    }
    // Fall back to compile-time creds.
    strlcpy(ssid, CONFIG_WIFI_SSID, ssid_len);
    strlcpy(pass, CONFIG_WIFI_PASSWORD, pass_len);
    return ssid[0] != '\0';
}

// Reconnect ownership: with the IDF roaming app enabled it reconnects after
// every ordinary disconnect (and retargets on a roam), indefinitely. Calling
// esp_wifi_connect() here as well would race it, so we only count failures and
// rely on the watchdog below as a backstop. Without the roaming app we retry
// ourselves, as before.
// Watchdog state is shared between the event task and the esp_timer task, so
// it is atomic and 32-bit: seconds since boot when the link (IP) was lost,
// 0 = up. Set on *every* disconnect, including driver-managed roams, so a roam
// that never completes is still recovered.
std::atomic<uint32_t> s_down_since_s{0};
esp_netif_t* s_sta_netif = nullptr;
esp_timer_handle_t s_link_wd = nullptr;

constexpr int64_t  kLinkWdPeriodUs = 30LL * 1000 * 1000;
constexpr uint32_t kLinkWdKickS    = 60;

uint32_t uptime_s() { return static_cast<uint32_t>(esp_timer_get_time() / 1000000) + 1; }

void link_watchdog_cb(void*) {
    const uint32_t down = s_down_since_s.load();
    if (down == 0 || uptime_s() - down < kLinkWdKickS) return;
    const unsigned long secs = static_cast<unsigned long>(uptime_s() - down);
    esp_err_t err;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        // Associated but no IP: esp_wifi_connect() would be a no-op, so kick DHCP.
        esp_err_t stop = esp_netif_dhcpc_stop(s_sta_netif);
        err = esp_netif_dhcpc_start(s_sta_netif);
        ESP_LOGW(TAG, "associated but no IP for %lus — restarting DHCP (%s/%s)",
                 secs, esp_err_to_name(stop), esp_err_to_name(err));
    } else {
        err = esp_wifi_connect();
        ESP_LOGW(TAG, "disconnected for %lus — kicking STA reconnect (%s)",
                 secs, esp_err_to_name(err));
    }
    // Give the kick a full window to complete before trying again (a slow
    // DHCP server must not be restarted mid-negotiation every period).
    if (err == ESP_OK) {
        uint32_t expected = down;
        s_down_since_s.compare_exchange_strong(expected, uptime_s());
    }
}
void on_event(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        // In provisioning (APSTA) the STA side is only used for scanning; don't
        // start a reconnect loop against credentials that just failed.
        if (s_mode != Mode::PROVISIONING) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_mode == Mode::PROVISIONING) return;
        uint32_t expected = 0;
        s_down_since_s.compare_exchange_strong(expected, uptime_s());
        auto* d = static_cast<wifi_event_sta_disconnected_t*>(data);
        // Driver-managed roam: the stack reconnects to the new AP itself, so
        // it is neither an outage nor a failed attempt.
        if (d && d->reason == WIFI_REASON_ROAMING) {
            ESP_LOGI(TAG, "roaming to another AP");
            return;
        }
        // Count a genuine outage only on the established->lost transition, not
        // on the retry storm that follows or on initial connect attempts.
        if (s_established) { s_wifi_drops++; s_established = false; }
        s_retries++;
        ESP_LOGW(TAG, "STA disconnected (reason %d, attempt %d)", d ? d->reason : -1, s_retries);
#if !CONFIG_ESP_WIFI_ENABLE_ROAMING_APP
        if (s_retries <= 5 || s_mode == Mode::CONNECTED) esp_wifi_connect();
#endif
        if (s_mode == Mode::CONNECTING && s_retries > 5) xEventGroupSetBits(s_events, kFailBit);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* e = static_cast<ip_event_got_ip_t*>(data);
        esp_ip4addr_ntoa(&e->ip_info.ip, s_ip, sizeof(s_ip));
        s_retries = 0;
        s_established = true;
        s_down_since_s.store(0);
        xEventGroupSetBits(s_events, kConnectedBit);
    }
}
// ── mDNS (called once STA is connected) ────────────────────────────────
// A unique hostname avoids ".local" collisions, but on its own it is not
// discoverable (nobody knows the suffix to type). So we also advertise a
// DNS-SD "_http._tcp" service with a unique instance name and TXT metadata —
// the standard Zeroconf/Bonjour pattern (ESPHome/Shelly/printers) that lets a
// controller browse and list every unit without guessing hostnames.
void start_mdns() {
    if (mdns_init() != ESP_OK) return;

    std::string uid      = device_uid();
    std::string host     = mdns_hostname();
    std::string instance = std::string("HeatLink ") + uid;
    const char* fw       = esp_app_get_description()->version;

    mdns_hostname_set(host.c_str());
    mdns_instance_name_set(instance.c_str());

    mdns_txt_item_t txt[] = {
        {"id",    uid.c_str()},
        {"fw",    fw},
        {"model", "stamp-s3"},
        {"path",  "/"},
    };
    mdns_service_add(nullptr, "_http", "_tcp", 80, txt,
                     sizeof(txt) / sizeof(txt[0]));

    // Also advertise a "_mmhvac._tcp" locator service so a controller (or a
    // sibling head) can browse for shared-compressor group candidates and read
    // their uid / protocol-version without guessing the hostname suffix. The
    // group-id is intentionally omitted — it changes at runtime after pairing;
    // enrolled peers are resolved by hostname (heatlink-<uid>) and
    // authenticated by HMAC, not discovered by group-id TXT.
    char pv[8];
    snprintf(pv, sizeof(pv), "%d", hvac_group::kProtocolVersion);
    mdns_txt_item_t gtxt[] = {
        {"uid", uid.c_str()},
        {"pv",  pv},
    };
    mdns_service_add(nullptr, "_mmhvac", "_tcp", 80, gtxt,
                     sizeof(gtxt) / sizeof(gtxt[0]));

    ESP_LOGI(TAG, "mDNS: %s.local  (_http._tcp instance '%s')",
             host.c_str(), instance.c_str());
}

// ── Captive-portal HTTP handlers ───────────────────────────────────────
esp_err_t portal_page(httpd_req_t* req) {
    size_t len = (size_t)(portal_html_gz_end - portal_html_gz_start);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, (const char*)portal_html_gz_start, len);
}

// OS captive-portal probes → 302 to the portal so the setup page pops up.
esp_err_t captive_redirect(httpd_req_t* req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

// GET /api/scan — blocking scan, returns [{ssid,rssi,auth}].
esp_err_t portal_scan(httpd_req_t* req) {
    std::string json;
    if (scan_json(json) != ESP_OK) json = "[]";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, json.c_str());
    return ESP_OK;
}

// A reboot is what finally applies the saved credentials and tears down the
// SoftAP. We defer it (rather than rebooting immediately in /api/connect) so
// the portal's success screen can stay live while the user copies the device
// URL. The reboot fires on whichever comes first: the user finishing setup
// (/api/finish, e.g. right after they tap Copy) or a device-side safety timer
// (in case the phone/captive sheet disappears and its JS timer never runs).
static esp_timer_handle_t s_reboot_timer = nullptr;
static void reboot_timer_cb(void*) { esp_restart(); }
static void schedule_reboot(uint32_t delay_ms) {
    if (!s_reboot_timer) {
        esp_timer_create_args_t a = {};
        a.callback = &reboot_timer_cb;
        a.dispatch_method = ESP_TIMER_TASK;
        a.name = "provisioning_reboot";
        esp_timer_create(&a, &s_reboot_timer);
    }
    esp_timer_stop(s_reboot_timer);  // harmless if not running; lets us reschedule
    esp_timer_start_once(s_reboot_timer, static_cast<uint64_t>(delay_ms) * 1000ULL);
}

// POST /api/connect — { "ssid": "...", "pass": "..." } → save creds, arm a
// safety reboot, and return the mDNS host so the portal can show its URL.
esp_err_t portal_connect(httpd_req_t* req) {
    char buf[256] = {};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    cJSON* json = cJSON_Parse(buf);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    cJSON* jssid = cJSON_GetObjectItem(json, "ssid");
    cJSON* jpass = cJSON_GetObjectItem(json, "pass");
    if (!jssid || !cJSON_IsString(jssid) || strlen(jssid->valuestring) == 0) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ssid");
        return ESP_FAIL;
    }
    const char* ssid = jssid->valuestring;
    const char* pass = (jpass && cJSON_IsString(jpass)) ? jpass->valuestring : "";

    // Optional friendly display name set during provisioning (sanitised in
    // set_display_name). Handy first-set moment when onboarding many units.
    cJSON* jname = cJSON_GetObjectItem(json, "name");
    if (jname && cJSON_IsString(jname)) set_display_name(jname->valuestring);

    ESP_LOGI(TAG, "portal: saving creds for '%s' (deferred reboot)", ssid);
    save_credentials(ssid, pass);
    cJSON_Delete(json);

    // Hand the portal page the exact mDNS hostname the device will come up on
    // after it reboots, so the success screen can show/copy/open that URL — no
    // IP lookup needed. The hostname is [a-z0-9-] only, so it needs no escaping.
    std::string resp = "{\"status\":\"saved\",\"host\":\"" + mdns_hostname() +
                       "\",\"message\":\"Ready to finish\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp.c_str());

    // Keep the AP (and this page) alive so the user isn't racing a reboot while
    // copying the URL. If they never signal /api/finish (e.g. the iOS captive
    // sheet closed and its JS timer stopped), reboot anyway after 3 minutes so
    // the device always comes up on the home network on its own.
    schedule_reboot(180000);
    return ESP_OK;
}

// POST /api/finish — the user is done with the success screen (typically right
// after copying the URL). Reboot promptly so the device joins the home network;
// a short delay lets this response flush before the SoftAP drops.
esp_err_t portal_finish(httpd_req_t* req) {
    ESP_LOGI(TAG, "portal: finish requested — rebooting");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"rebooting\"}");
    schedule_reboot(800);
    return ESP_OK;
}

esp_err_t start_provisioning() {
    s_mode = Mode::PROVISIONING;

    // AP name carries the device_uid so multiple units are distinct and the
    // SoftAP, mDNS hostname and MQTT node all share one suffix.
    std::snprintf(s_ap_name, sizeof(s_ap_name), "%s-%s",
                  CONFIG_MDNS_HOSTNAME, device_uid().c_str());

    esp_netif_create_default_wifi_ap();

    wifi_config_t ap = {};
    strlcpy(reinterpret_cast<char*>(ap.ap.ssid), s_ap_name, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = std::strlen(s_ap_name);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;

    // APSTA so we can scan for networks while serving the portal.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    apply_tx_power_cap();

    start_dns_server();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 16;
    cfg.stack_size = 8192;
    cfg.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&s_portal, &cfg) == ESP_OK) {
        const httpd_uri_t page = {"/", HTTP_GET, portal_page, nullptr};
        httpd_register_uri_handler(s_portal, &page);
        const httpd_uri_t scan = {"/api/scan", HTTP_GET, portal_scan, nullptr};
        httpd_register_uri_handler(s_portal, &scan);
        const httpd_uri_t conn = {"/api/connect", HTTP_POST, portal_connect, nullptr};
        httpd_register_uri_handler(s_portal, &conn);
        const httpd_uri_t fin = {"/api/finish", HTTP_POST, portal_finish, nullptr};
        httpd_register_uri_handler(s_portal, &fin);

        static const char* probes[] = {
            "/generate_204", "/gen_204", "/hotspot-detect.html",
            "/library/test/success.html", "/connecttest.txt",
            "/redirect", "/ncsi.txt",
        };
        for (auto* p : probes) {
            const httpd_uri_t r = {p, HTTP_GET, captive_redirect, nullptr};
            httpd_register_uri_handler(s_portal, &r);
        }
        const httpd_uri_t catchall = {"/*", HTTP_GET, captive_redirect, nullptr};
        httpd_register_uri_handler(s_portal, &catchall);

        ESP_LOGW(TAG, "captive portal up at http://192.168.4.1/ (SSID '%s')", s_ap_name);
    }
    return ESP_ERR_NOT_FINISHED;
}
}  // namespace

// ── Pairing discovery (public) ─────────────────────────────────────────
// While an "add a zone" pairing window is open, the owning head advertises
// pair=1 in its _mmhvac._tcp TXT (plus the human-readable group label and its
// own display name) so an ungrouped head can *discover* the pairing session
// without the user typing an address. The TXT is cleared the moment the window
// closes (join/cancel/expiry/lockout) — mDNS announces the update so browsers
// stop offering it promptly. The pairing code itself is NEVER advertised; it is
// only ever exchanged over the authenticated claim, so pair=1 leaks nothing but
// "this head is currently accepting one new member".
// The House-view identity (name/ghash/glabel) is always advertised, so the
// pairing advert only toggles pair=1 and, while active, the label/name the
// pairing UI shows; closing the window restores the house values.
static std::mutex  s_advert_mtx;
static std::string s_house_name, s_house_ghash, s_house_glabel;

static void txt_set_or_remove(const char* key, const std::string& v) {
    if (!v.empty()) mdns_service_txt_item_set("_mmhvac", "_tcp", key, v.c_str());
    else            mdns_service_txt_item_remove("_mmhvac", "_tcp", key);
}

static bool s_pairing_active = false;

void set_pairing_advert(bool active, const std::string& glabel,
                        const std::string& name) {
    std::lock_guard<std::mutex> lk(s_advert_mtx);
    s_pairing_active = active;
    if (active) {
        mdns_service_txt_item_set("_mmhvac", "_tcp", "pair", "1");
        txt_set_or_remove("glabel", glabel);
        txt_set_or_remove("name", name);
    } else {
        mdns_service_txt_item_remove("_mmhvac", "_tcp", "pair");
        txt_set_or_remove("glabel", s_house_glabel);
        txt_set_or_remove("name", s_house_name);
    }
}

void set_house_advert(const std::string& name, const std::string& ghash,
                      const std::string& glabel) {
    std::lock_guard<std::mutex> lk(s_advert_mtx);
    if (name == s_house_name && ghash == s_house_ghash && glabel == s_house_glabel)
        return;
    s_house_name = name;
    s_house_ghash = ghash;
    s_house_glabel = glabel;
    txt_set_or_remove("ghash", ghash);
    if (!s_pairing_active) {
        txt_set_or_remove("glabel", glabel);
        txt_set_or_remove("name", name);
    }
}

// First IPv4 address of an mDNS result as dotted quad; "" if none.
static std::string result_ipv4(const mdns_result_t* r) {
    for (mdns_ip_addr_t* a = r->addr; a; a = a->next) {
        if (a->addr.type == ESP_IPADDR_TYPE_V4) {
            uint32_t v = a->addr.u_addr.ip4.addr;  // LSB = first octet
            char ip[16];
            snprintf(ip, sizeof(ip), "%u.%u.%u.%u",
                     (unsigned)(v & 0xff), (unsigned)((v >> 8) & 0xff),
                     (unsigned)((v >> 16) & 0xff), (unsigned)((v >> 24) & 0xff));
            return ip;
        }
    }
    return "";
}

// Browse the LAN for heads currently advertising pair=1 and return a JSON array
// [{"uid":..,"name":..,"glabel":..,"host":"<ip>"}], excluding ourselves. The
// caller (the ungrouped head's web UI) presents this list so the user taps the
// intended group — the code is then sent to that ONE selected host only, never
// fanned out. Bounded to a handful of results with a short query timeout.
esp_err_t discover_pairing(std::string& out_json) {
    mdns_result_t* results = nullptr;
    esp_err_t err = mdns_query_ptr("_mmhvac", "_tcp", 2500, 20, &results);
    if (err != ESP_OK) return err;

    const std::string self_uid = device_uid();
    cJSON* arr = cJSON_CreateArray();
    for (mdns_result_t* r = results; r; r = r->next) {
        // Read TXT: require pair=1; collect uid/name/glabel.
        bool pairing = false;
        std::string uid, name, glabel;
        for (size_t i = 0; i < r->txt_count; i++) {
            const char* k = r->txt[i].key;
            const char* v = r->txt[i].value ? r->txt[i].value : "";
            if (!k) continue;
            if (!strcmp(k, "pair"))        pairing = (!strcmp(v, "1"));
            else if (!strcmp(k, "uid"))    uid = v;
            else if (!strcmp(k, "name"))   name = v;
            else if (!strcmp(k, "glabel")) glabel = v;
        }
        if (!pairing) continue;
        if (!uid.empty() && uid == self_uid) continue;  // never offer ourselves

        std::string ip = result_ipv4(r);
        if (ip.empty()) continue;  // no usable address

        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "uid",    uid.c_str());
        cJSON_AddStringToObject(o, "name",   name.c_str());
        cJSON_AddStringToObject(o, "glabel", glabel.c_str());
        cJSON_AddStringToObject(o, "host",   ip.c_str());
        cJSON_AddItemToArray(arr, o);
    }
    mdns_query_results_free(results);

    char* s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!s) return ESP_ERR_NO_MEM;
    out_json = s;
    cJSON_free(s);
    return ESP_OK;
}

esp_err_t browse_heads(std::vector<HeadAdvert>& out) {
    out.clear();
    mdns_result_t* results = nullptr;
    esp_err_t err = mdns_query_ptr("_mmhvac", "_tcp", 2500, 20, &results);
    if (err != ESP_OK) return err;

    const std::string self_uid = device_uid();
    for (mdns_result_t* r = results; r; r = r->next) {
        HeadAdvert h;
        for (size_t i = 0; i < r->txt_count; i++) {
            const char* k = r->txt[i].key;
            const char* v = r->txt[i].value ? r->txt[i].value : "";
            if (!k) continue;
            if (!strcmp(k, "uid"))         h.uid = v;
            else if (!strcmp(k, "name"))   h.name = v;
            else if (!strcmp(k, "ghash"))  h.ghash = v;
            else if (!strcmp(k, "glabel")) h.glabel = v;
            else if (!strcmp(k, "pv"))     h.pv = atoi(v);
        }
        if (h.uid.empty() || h.uid == self_uid) continue;
        h.ip = result_ipv4(r);
        if (h.ip.empty()) continue;
        bool dup = false;
        for (const auto& e : out) if (e.uid == h.uid) { dup = true; break; }
        if (!dup) out.push_back(std::move(h));
    }
    mdns_query_results_free(results);
    return ESP_OK;
}

// ── Identity (public) ──────────────────────────────────────────────────
// Short, stable per-chip id from the low 2 bytes of the factory base MAC
// (Espressif assigns every chip a globally-unique MAC, so this is collision-free
// for a handful of units). Reused for the SoftAP name, the mDNS hostname, and the
// default MQTT friendly_name so all three are unique and consistent.
std::string device_uid() {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char buf[5];
    std::snprintf(buf, sizeof(buf), "%02x%02x", mac[4], mac[5]);
    return std::string(buf);
}

// DHCP hostname (what the router / UniFi shows instead of "espressif"):
// "HeatLink-<room>" from the display name, e.g. "HeatLink-Master-Bedroom".
// Hostnames are limited to letters, digits and '-' (RFC 1123), so accented
// Latin letters are folded to ASCII ("Éric" -> "Eric"), any other run of
// characters becomes one '-' (apostrophes are dropped), and an over-long name is cut at a word boundary
// to fit esp_netif's 32-byte limit. Falls back to the mDNS name.
// ASCII fold for U+00C0..U+00FF (UTF-8 lead byte 0xC3); '\0' = no letter.
static const char kLatin1Fold[] =
    "AAAAAAACEEEEIIII" "DNOOOOO\0OUUUUY\0s"   // U+00C0..U+00DF (Æ->A, ×, Þ, ß->s)
    "aaaaaaaceeeeiiii" "dnooooo\0ouuuuy\0y";  // U+00E0..U+00FF (æ->a, ÷, þ)

static std::string dhcp_hostname(const std::string& name) {
    constexpr size_t kMax = 32;
    std::string h = "HeatLink-";
    const size_t base = h.size();
    auto sep = [&] { if (h.size() > base && h.back() != '-') h += '-'; };
    for (size_t i = 0; i < name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            h += static_cast<char>(c);
        } else if (c == 0xC3 && i + 1 < name.size() &&
                   (static_cast<unsigned char>(name[i + 1]) & 0xC0) == 0x80) {
            const char a = kLatin1Fold[(static_cast<unsigned char>(name[i + 1]) & 0x3F)];
            ++i;
            if (a) h += a; else sep();
        } else if (c == 0xC5 && i + 1 < name.size() &&
                   (name[i + 1] == '\x92' || name[i + 1] == '\x93')) {
            h += (name[i + 1] == '\x92') ? "OE" : "oe";  // Œ / œ
            ++i;
        } else if (c == '\'') {
            // Drop apostrophes: "Eric's" -> "Erics".
        } else if (c == 0xE2 && i + 2 < name.size() && name[i + 1] == '\x80' &&
                   (name[i + 2] == '\x98' || name[i + 2] == '\x99')) {
            i += 2;  // typographic apostrophe U+2018/U+2019
        } else if (c >= 0x80) {
            // Skip the rest of any other UTF-8 sequence, then separate.
            while (i + 1 < name.size() && (static_cast<unsigned char>(name[i + 1]) & 0xC0) == 0x80) ++i;
            sep();
        } else {
            sep();
        }
    }
    while (h.size() > base && h.back() == '-') h.pop_back();
    if (h.size() > kMax) {
        const size_t cut = h.rfind('-', kMax);  // last word break that fits
        h.resize(cut != std::string::npos && cut > base ? cut : kMax);
        while (h.size() > base && h.back() == '-') h.pop_back();
    }
    if (h.size() <= base) return std::string(CONFIG_MDNS_HOSTNAME) + "-" + device_uid();
    return h;
}

static void apply_dhcp_hostname(const std::string& name) {
    if (!s_sta_netif) return;
    const std::string h = dhcp_hostname(name);
    esp_err_t err = esp_netif_set_hostname(s_sta_netif, h.c_str());
    if (err != ESP_OK) ESP_LOGW(TAG, "set hostname '%s' failed: %s", h.c_str(), esp_err_to_name(err));
    else ESP_LOGI(TAG, "DHCP hostname '%s'", h.c_str());
}

// DHCP option 60 so routers can identify the device type. Must be set
// before the DHCP client starts (i.e. before esp_wifi_start()).
static void apply_dhcp_vendor_class() {
    static char vc[] = "HeatLink";
    esp_err_t err = esp_netif_dhcpc_option(s_sta_netif, ESP_NETIF_OP_SET,
                                           ESP_NETIF_VENDOR_CLASS_IDENTIFIER, vc, sizeof(vc) - 1);
    if (err != ESP_OK) ESP_LOGW(TAG, "set DHCP vendor class failed: %s", esp_err_to_name(err));
}
std::string mdns_hostname() {
    return std::string(CONFIG_MDNS_HOSTNAME) + "-" + device_uid();
}

std::string device_display_name() {
    if (!s_name_mtx) return s_name;  // pre-init (shouldn't happen post-boot)
    xSemaphoreTake(s_name_mtx, portMAX_DELAY);
    std::string copy = s_name;
    xSemaphoreGive(s_name_mtx);
    return copy;
}

esp_err_t set_display_name(const char* name) {
    std::string n = sanitize_name(name);
    nvs_handle_t h;
    esp_err_t err = nvs_open(kDeviceNvsNs, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, "name", n.c_str());
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;  // keep the old cached value on failure
    xSemaphoreTake(s_name_mtx, portMAX_DELAY);
    s_name = n;
    xSemaphoreGive(s_name_mtx);
    apply_dhcp_hostname(n);  // routers see it after the next reconnect/reboot
    return ESP_OK;
}

bool temp_unit_fahrenheit() {
    if (!s_name_mtx) return s_temp_f;
    xSemaphoreTake(s_name_mtx, portMAX_DELAY);
    bool v = s_temp_f;
    xSemaphoreGive(s_name_mtx);
    return v;
}

esp_err_t set_temp_unit(bool fahrenheit) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(kDeviceNvsNs, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, "tunit", fahrenheit ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;  // keep the old cached value on failure
    if (s_name_mtx) xSemaphoreTake(s_name_mtx, portMAX_DELAY);
    s_temp_f = fahrenheit;
    if (s_name_mtx) xSemaphoreGive(s_name_mtx);
    return ESP_OK;
}

std::string ui_language() {
    if (!s_name_mtx) return s_ui_lang;
    xSemaphoreTake(s_name_mtx, portMAX_DELAY);
    std::string v = s_ui_lang;
    xSemaphoreGive(s_name_mtx);
    return v;
}

esp_err_t set_ui_language(const char* lang) {
    // Keep only a short, sane code: letters, digits and '-', capped at 15 bytes.
    // Empty ("") is valid and means "auto" (browser-detected).
    std::string n;
    if (lang) {
        for (const char* p = lang; *p && n.size() < 15; ++p) {
            char c = *p;
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-')
                n += c;
        }
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(kDeviceNvsNs, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, "lang", n.c_str());
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;  // keep the old cached value on failure
    if (s_name_mtx) xSemaphoreTake(s_name_mtx, portMAX_DELAY);
    s_ui_lang = n;
    if (s_name_mtx) xSemaphoreGive(s_name_mtx);
    return ESP_OK;
}

int get_tx_power_dbm() { return s_tx_power_dbm; }
int tx_power_min_dbm() { return kMinTxPowerDbm; }
int tx_power_max_dbm() { return kMaxTxPowerDbm; }

esp_err_t set_tx_power_dbm(int dbm) {
    if (dbm < kMinTxPowerDbm) dbm = kMinTxPowerDbm;
    if (dbm > kMaxTxPowerDbm) dbm = kMaxTxPowerDbm;
    nvs_handle_t h;
    esp_err_t err = nvs_open(kDeviceNvsNs, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_i8(h, "txpow", static_cast<int8_t>(dbm));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;  // keep the old cached value on failure
    s_tx_power_dbm = static_cast<int8_t>(dbm);
    apply_tx_power_cap();           // live — no reboot needed
    return ESP_OK;
}

esp_err_t init() {
    s_events = xEventGroupCreate();
    s_name_mtx = xSemaphoreCreateMutex();
    load_name();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_event, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_event, nullptr, nullptr));

    char ssid[33] = {0}, pass[65] = {0};
    if (!load_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGW(TAG, "no credentials — entering provisioning");
        return start_provisioning();
    }
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    strlcpy(s_pass, pass, sizeof(s_pass));

    s_mode = Mode::CONNECTING;
    s_sta_netif = esp_netif_create_default_wifi_sta();
    apply_dhcp_hostname(device_display_name());
    apply_dhcp_vendor_class();

    wifi_config_t sta = {};
    strlcpy(reinterpret_cast<char*>(sta.sta.ssid), ssid, sizeof(sta.sta.ssid));
    strlcpy(reinterpret_cast<char*>(sta.sta.password), pass, sizeof(sta.sta.password));
    // Default is a fast scan that joins the first matching AP found. On a
    // multi-AP network that can be a distant one, so scan all channels and
    // pick the strongest.
    sta.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    sta.sta.rm_enabled  = 1;  // 802.11k neighbour reports
    sta.sta.btm_enabled = 1;  // 802.11v BSS transition requests

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());
    apply_tx_power_cap();

    ESP_LOGI(TAG, "connecting to '%s'…", ssid);
    EventBits_t bits = xEventGroupWaitBits(
        s_events, kConnectedBit | kFailBit, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(CONFIG_WIFI_CONNECT_TIMEOUT_S * 1000));

    if (bits & kConnectedBit) {
        s_mode = Mode::CONNECTED;
        ESP_LOGI(TAG, "connected, IP %s", s_ip);
        start_mdns();
        esp_timer_create_args_t wd = {};
        wd.callback = &link_watchdog_cb;
        wd.name = "wifi_wd";
        if (esp_timer_create(&wd, &s_link_wd) == ESP_OK) esp_timer_start_periodic(s_link_wd, kLinkWdPeriodUs);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "STA connect failed — entering provisioning");
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_NULL);
    return start_provisioning();
}

Mode get_mode() { return s_mode; }
bool is_connected() { return s_mode == Mode::CONNECTED; }
const char* get_ip() { return s_ip; }
const char* get_ap_name() { return s_ap_name; }
const char* get_ssid() { return s_ssid; }
bool has_password() { return s_pass[0] != '\0'; }
const char* get_password() { return s_pass; }

uint32_t disconnect_count() { return s_wifi_drops; }
void reset_disconnect_count() { s_wifi_drops = 0; }

// Live RSSI (dBm) of the current STA association, or 0 when not connected.
// Queries the driver each call so callers see the current signal, not the
// value captured at connect time.
int get_rssi() {
    if (s_mode != Mode::CONNECTED) return 0;
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return 0;
    return ap.rssi;
}

int get_auth() {
    if (s_mode != Mode::CONNECTED) return -1;
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return -1;
    return ap.authmode;
}

esp_err_t scan_json(std::string& out) {
    wifi_scan_config_t scan_cfg = {};
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);  // blocking
    if (err != ESP_OK) return err;
    s_scan_count = sizeof(s_scan) / sizeof(s_scan[0]);
    esp_wifi_scan_get_ap_records(&s_scan_count, s_scan);

    // Dedup by SSID keeping the strongest record, drop hidden/empty, sort desc.
    struct Net { std::string ssid; int8_t rssi; uint8_t auth; };
    std::vector<Net> nets;
    for (int i = 0; i < s_scan_count; i++) {
        const char* ssid = reinterpret_cast<const char*>(s_scan[i].ssid);
        if (ssid[0] == '\0') continue;
        auto it = std::find_if(nets.begin(), nets.end(),
                               [&](const Net& n) { return n.ssid == ssid; });
        if (it == nets.end()) {
            nets.push_back({ssid, s_scan[i].rssi,
                            static_cast<uint8_t>(s_scan[i].authmode)});
        } else if (s_scan[i].rssi > it->rssi) {
            it->rssi = s_scan[i].rssi;
            it->auth = static_cast<uint8_t>(s_scan[i].authmode);
        }
    }
    std::sort(nets.begin(), nets.end(),
              [](const Net& a, const Net& b) { return a.rssi > b.rssi; });

    cJSON* arr = cJSON_CreateArray();
    for (const auto& n : nets) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", n.ssid.c_str());
        cJSON_AddNumberToObject(o, "rssi", n.rssi);
        cJSON_AddNumberToObject(o, "auth", n.auth);
        cJSON_AddItemToArray(arr, o);
    }
    char* str = cJSON_PrintUnformatted(arr);
    out = str ? str : "[]";
    cJSON_free(str);
    cJSON_Delete(arr);
    return ESP_OK;
}

esp_err_t save_credentials(const char* ssid, const char* pass) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNvsNs, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t erase_credentials() {
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNvsNs, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_erase_all(h);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

}  // namespace wifi
