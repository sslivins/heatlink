/// @file web_ui.cpp
/// @brief On-device diagnostics/control web server (implementation).

#include "web_ui.h"
#include "access_policy.h"
#include "park_ctl.h"
#include "room_sensor_ctl.h"
#include "slice_ctl.h"
#include "wifi_manager.h"
#include "ota.h"
#include "auth_manager.h"
#include "group_config.h"
#include "group_proto.h"
#include "status_led.h"
#include "devlog.h"
#include "events.h"
#include "history_ctl.h"
#include "capability.h"
#include "factory_reset.h"
#include "diag.h"
#include "token_store.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <strings.h>
#include <map>
#include <new>
#include <string>
#include <vector>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "mdns.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char* TAG = "web";

// Embedded gzip'd dashboard — produced at configure time by gzip_html.py.
extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[]   asm("_binary_index_html_gz_end");

// Embedded PWA icons + web app manifest (served raw; PNGs are already compressed).
extern const uint8_t icon_180_png_start[] asm("_binary_icon_180_png_start");
extern const uint8_t icon_180_png_end[]   asm("_binary_icon_180_png_end");
extern const uint8_t icon_192_png_start[] asm("_binary_icon_192_png_start");
extern const uint8_t icon_192_png_end[]   asm("_binary_icon_192_png_end");
extern const uint8_t icon_512_png_start[] asm("_binary_icon_512_png_start");
extern const uint8_t icon_512_png_end[]   asm("_binary_icon_512_png_end");
extern const uint8_t manifest_start[]     asm("_binary_manifest_webmanifest_start");
extern const uint8_t manifest_end[]       asm("_binary_manifest_webmanifest_end");

namespace web_ui {

namespace {

// Bumped on breaking changes to the REST API (see GET /api/info).
constexpr int kApiVersion = 1;

httpd_handle_t s_server = nullptr;
Hooks          s_hooks;

void set_cors(httpd_req_t* req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    // API responses must never be cached. A stale /api/auth in particular
    // would keep the login gate up (or hidden) even after the real state
    // changed, producing a "stuck on the login page" loop.
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

void refresh_house_advert();  // defined with the House view below

// ── Auth middleware ────────────────────────────────────────────────────
constexpr const char* kCookieName = "mitsubishi_session";

void send_unauthorized(httpd_req_t* req) {
    set_cors(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_sendstr(req, "{\"error\":\"unauthorized\"}");
}

// Pull the session token out of the Cookie header, if present.
bool get_cookie_token(httpd_req_t* req, char* out, size_t out_len) {
    size_t hlen = httpd_req_get_hdr_value_len(req, "Cookie");
    if (hlen == 0) return false;
    char* cookie = (char*)malloc(hlen + 1);
    if (!cookie) return false;
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, hlen + 1) != ESP_OK) {
        free(cookie);
        return false;
    }
    bool found = false;
    char needle[40];
    snprintf(needle, sizeof(needle), "%s=", kCookieName);
    char* p = strstr(cookie, needle);
    if (p) {
        p += strlen(needle);
        size_t i = 0;
        while (p[i] && p[i] != ';' && p[i] != ' ' && i < out_len - 1) {
            out[i] = p[i];
            i++;
        }
        out[i] = '\0';
        found = i > 0;
    }
    free(cookie);
    return found;
}

bool get_api_key(httpd_req_t* req, char* out, size_t out_len) {
    size_t hlen = httpd_req_get_hdr_value_len(req, "X-API-Key");
    if (hlen == 0 || hlen >= out_len) return false;
    return httpd_req_get_hdr_value_str(req, "X-API-Key", out, out_len) == ESP_OK;
}

// Authorize a data/control request. Web login is the gate for human/browser
// access to the UI and the data endpoints it calls; when it's disabled the UI
// is open, so these endpoints are too — regardless of the API-key setting,
// which can't distinguish a browser from a script on these shared routes.
// (Mirrors admin_authorized's "no web login → full access".) When web login is
// on, accept a valid session cookie OR, if API auth is on, a valid X-API-Key.
// Climate control is open to anyone on the LAN when the head is claimed but no
// climate-only password is set: visitors without a session get the "user" role.
static bool open_climate() {
    return auth_mgr_state() == AUTH_STATE_CLAIMED && !auth_mgr_user_password_set();
}

// Integration token from "Authorization: Bearer <token>".
enum class TokenAuth { None, Bad, Ok };
TokenAuth bearer_token(httpd_req_t* req, tokens::Entry* e = nullptr) {
    char hdr[96];
    size_t hlen = httpd_req_get_hdr_value_len(req, "Authorization");
    if (hlen == 0) return TokenAuth::None;
    if (hlen >= sizeof(hdr) ||
        httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK ||
        strncasecmp(hdr, "Bearer ", 7) != 0)
        return TokenAuth::Bad;
    const char* t = hdr + 7;
    while (*t == ' ') ++t;
    tokens::Entry tmp;
    return token_store_validate(t, e ? e : &tmp) ? TokenAuth::Ok : TokenAuth::Bad;
}

// A presented token is judged on its own: a bad one is refused even where the
// endpoint would otherwise be open, so an integration learns it must re-auth.
// Tokens work whether or not the legacy API-key toggle is on.
bool api_authorized(httpd_req_t* req) {
    switch (bearer_token(req)) {
        case TokenAuth::Ok: return true;
        case TokenAuth::Bad: return false;
        case TokenAuth::None: break;
    }
    if (!auth_mgr_web_auth_enabled()) return true;
    if (open_climate()) return true;
    char tok[AUTH_SESSION_TOKEN_LEN + 1];
    if (get_cookie_token(req, tok, sizeof(tok)) && auth_mgr_validate_session(tok))
        return true;
    if (auth_mgr_api_auth_enabled()) {
        char key[AUTH_API_KEY_LEN + 1];
        if (get_api_key(req, key, sizeof(key)) && auth_mgr_validate_api_key(key))
            return true;
    }
    return false;
}

// True if the request is allowed admin-level access. The climate-only "user"
// role is denied. An API key (when API auth is on) and a web-auth-disabled
// device both count as admin.
bool admin_authorized(httpd_req_t* req) {
    tokens::Entry te;
    switch (bearer_token(req, &te)) {
        case TokenAuth::Ok: return te.scope == tokens::Scope::Admin;
        case TokenAuth::Bad: return false;
        case TokenAuth::None: break;
    }
    if (!auth_mgr_web_auth_enabled()) return true;  // no web login → full access
    if (auth_mgr_api_auth_enabled()) {
        char key[AUTH_API_KEY_LEN + 1];
        if (get_api_key(req, key, sizeof(key)) && auth_mgr_validate_api_key(key))
            return true;
    }
    char tok[AUTH_SESSION_TOKEN_LEN + 1];
    return get_cookie_token(req, tok, sizeof(tok)) &&
           auth_mgr_session_role(tok) == AUTH_ROLE_ADMIN;
}

// Dotted IPv4 of the requesting client (IPv4-mapped IPv6 unwrapped), or "".
std::string peer_ipv4(httpd_req_t* req) {
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) return "";
    struct sockaddr_storage ss;
    socklen_t l = sizeof(ss);
    if (getpeername(fd, (struct sockaddr*)&ss, &l) != 0) return "";
    char ip[16] = "";
    if (ss.ss_family == AF_INET) {
        inet_ntop(AF_INET, &((struct sockaddr_in*)&ss)->sin_addr, ip, sizeof(ip));
    } else if (ss.ss_family == AF_INET6) {
        const uint8_t* b = ((struct sockaddr_in6*)&ss)->sin6_addr.s6_addr;
        static const uint8_t kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        if (memcmp(b, kMapped, 12) == 0) inet_ntop(AF_INET, b + 12, ip, sizeof(ip));
    }
    return ip;
}

// Human-readable label for whoever made a web-UI request, for the activity
// log. Empty when web auth is disabled (the UI is anonymous). Admin resolves to
// the configured username; the shared climate-only role logs as "climate user".
std::string web_actor_name(httpd_req_t* req) {
    tokens::Entry te;
    if (bearer_token(req, &te) == TokenAuth::Ok) return te.label;
    if (!auth_mgr_web_auth_enabled()) return "";
    char tok[AUTH_SESSION_TOKEN_LEN + 1];
    if (get_cookie_token(req, tok, sizeof(tok))) {
        if (auth_mgr_session_role(tok) == AUTH_ROLE_ADMIN) {
            const char* u = auth_mgr_get_username();
            return (u && u[0]) ? std::string(u) : std::string("admin");
        }
        if (auth_mgr_session_role(tok) == AUTH_ROLE_USER) return "climate user";
    }
    if (open_climate()) return "guest";
    return "";  // API key / script
}

// Title-case a HVAC mode/fan token ("HEAT" -> "Heat", "fan_only" -> "Fan only")
// for log messages. Leaves the input untouched apart from case + underscores.
std::string pretty_token(const char* s) {
    std::string out;
    if (!s) return out;
    bool start = true;
    for (const char* p = s; *p; ++p) {
        char c = *p;
        if (c == '_') { out += ' '; start = true; continue; }
        if (start) { out += (char)toupper((unsigned char)c); start = false; }
        else out += (char)tolower((unsigned char)c);
    }
    return out;
}

void set_session_cookie(httpd_req_t* req, const char* token) {
    // HTTP-only device (no TLS), so the Secure flag is intentionally omitted.
    // NOTE: httpd_resp_set_hdr() stores the pointer WITHOUT copying, so the
    // buffer must stay valid until the response is sent. A stack buffer here
    // would dangle the moment this function returns (the caller then reuses
    // that stack for its JSON body, corrupting the Set-Cookie header). The
    // httpd request handler runs single-threaded, so a static buffer is safe.
    static char hdr[160];
    snprintf(hdr, sizeof(hdr),
             "%s=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=%ld",
             kCookieName, token, (long)AUTH_SESSION_LIFETIME_SEC);
    httpd_resp_set_hdr(req, "Set-Cookie", hdr);
}

void clear_session_cookie(httpd_req_t* req) {
    static char hdr[120];  // must outlive the response — see set_session_cookie
    snprintf(hdr, sizeof(hdr),
             "%s=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0", kCookieName);
    httpd_resp_set_hdr(req, "Set-Cookie", hdr);
}

// Guard helper used at the top of protected handlers.
void send_forbidden(httpd_req_t* req, const char* msg) {
    set_cors(req);
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_set_type(req, "application/json");
    char body[96];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", msg);
    httpd_resp_sendstr(req, body);
}

// A read-scoped token may only make GET requests.
bool token_scope_denies(httpd_req_t* req) {
    if (req->method == HTTP_GET) return false;
    tokens::Entry te;
    return bearer_token(req, &te) == TokenAuth::Ok && te.scope == tokens::Scope::Read;
}

#define REQUIRE_API_AUTH(req)                                                  \
    do {                                                                       \
        if (!api_authorized(req)) { send_unauthorized(req); return ESP_FAIL; } \
        if (token_scope_denies(req)) {                                         \
            send_forbidden(req, "token is read-only");                         \
            return ESP_FAIL;                                                   \
        }                                                                      \
    } while (0)

// Guard for admin-only handlers: requires API auth AND an admin-level role
// (the climate-only "user" gets a 403).
#define REQUIRE_ADMIN(req)                                                  \
    do {                                                                    \
        if (!api_authorized(req)) { send_unauthorized(req); return ESP_FAIL; } \
        if (!admin_authorized(req)) {                                       \
            send_forbidden(req, "admin access required");                   \
            return ESP_FAIL;                                                \
        }                                                                   \
    } while (0)


// ── GET / ──────────────────────────────────────────────────────────────
esp_err_t handle_root(httpd_req_t* req) {
    size_t len = (size_t)(index_html_gz_end - index_html_gz_start);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char*)index_html_gz_start, len);
}

// ── PWA icons + manifest (public, cached) ──────────────────────────────
static esp_err_t send_asset(httpd_req_t* req, const uint8_t* start,
                            const uint8_t* end, const char* ctype) {
    httpd_resp_set_type(req, ctype);
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=604800");
    return httpd_resp_send(req, (const char*)start, (size_t)(end - start));
}
esp_err_t handle_icon_180(httpd_req_t* req) {
    return send_asset(req, icon_180_png_start, icon_180_png_end, "image/png");
}
esp_err_t handle_icon_192(httpd_req_t* req) {
    return send_asset(req, icon_192_png_start, icon_192_png_end, "image/png");
}
esp_err_t handle_icon_512(httpd_req_t* req) {
    return send_asset(req, icon_512_png_start, icon_512_png_end, "image/png");
}
esp_err_t handle_manifest(httpd_req_t* req) {
    return send_asset(req, manifest_start, manifest_end, "application/manifest+json");
}

// ── GET /api/status ────────────────────────────────────────────────────
esp_err_t handle_status(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_API_AUTH(req);
    const esp_app_desc_t* app = esp_app_get_description();
    PowerTelemetry pwr = s_hooks.get_power ? s_hooks.get_power() : PowerTelemetry{};

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", app->version);
    cJSON_AddStringToObject(root, "hostname", wifi::mdns_hostname().c_str());
    cJSON_AddStringToObject(root, "name", wifi::device_display_name().c_str());
    cJSON_AddStringToObject(root, "ip", wifi::get_ip());
    cJSON_AddStringToObject(root, "ssid", wifi::get_ssid());
    cJSON_AddNumberToObject(root, "rssi", wifi::get_rssi());
    cJSON_AddNumberToObject(root, "tx_power_dbm", wifi::get_tx_power_dbm());
    cJSON_AddNumberToObject(root, "tx_power_min_dbm", wifi::tx_power_min_dbm());
    cJSON_AddNumberToObject(root, "tx_power_max_dbm", wifi::tx_power_max_dbm());
    cJSON_AddBoolToObject(root, "unit_connected",
                          s_hooks.unit_connected && s_hooks.unit_connected());
    cJSON_AddBoolToObject(root, "mqtt_connected",
                          s_hooks.mqtt_connected && s_hooks.mqtt_connected());
    cJSON_AddBoolToObject(root, "mqtt_configured",
                          !hvac_mqtt::get_settings().host.empty());
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(root, "free_heap", (double)esp_get_free_heap_size());

    cJSON* p = cJSON_CreateObject();
    cJSON_AddBoolToObject(p, "present", pwr.present);
    cJSON_AddNumberToObject(p, "vbat_mv", pwr.vbat_mv);
    cJSON_AddNumberToObject(p, "vin_mv", pwr.vin_mv);
    cJSON_AddNumberToObject(p, "vinout_mv", pwr.vinout_mv);
    cJSON_AddNumberToObject(p, "input_mv", pwr.input_mv);
    cJSON_AddStringToObject(p, "source", pwr.source);
    cJSON_AddBoolToObject(p, "charging", pwr.charging);
    cJSON_AddBoolToObject(p, "battery_present", pwr.battery_present);
    cJSON_AddItemToObject(root, "power", p);

    DiagTelemetry dg = s_hooks.get_diag ? s_hooks.get_diag() : DiagTelemetry{};
    cJSON* d = cJSON_CreateObject();
    cJSON_AddNumberToObject(d, "boot_count", dg.boot_count);
    cJSON_AddNumberToObject(d, "brownout_count", dg.brownout_count);
    cJSON_AddStringToObject(d, "reset_reason", dg.reset_reason);
    cJSON_AddBoolToObject(d, "last_was_brownout", dg.last_was_brownout);
    cJSON_AddNumberToObject(d, "vin_min_mv", dg.vin_min_mv);
    cJSON_AddNumberToObject(d, "vin_min_ever_mv", dg.vin_min_ever_mv);
    cJSON_AddNumberToObject(d, "vin_sag_count", dg.vin_sag_count);
    cJSON_AddNumberToObject(d, "wifi_drop_count", dg.wifi_drop_count);
    cJSON_AddNumberToObject(d, "wifi_drops_24h", wifi::drops_last_24h());
    cJSON_AddItemToObject(root, "diag", d);

    char* str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, str);
    cJSON_free(str);
    cJSON_Delete(root);
    return ESP_OK;
}

static void add_num_or_null(cJSON* o, const char* k, bool has, double v) {
    if (has) cJSON_AddNumberToObject(o, k, v);
    else cJSON_AddNullToObject(o, k);
}

// Room temperature for displays: the wall sensors' average (0.1 °C) while any
// is fresh, otherwise the unit's reading. *rs = "" / "ok" / "stale".
float display_room_temp(const cn105::Status& sta, const char** rs) {
    const room_sensor_ctl::Summary s = room_sensor_ctl::summary();
    *rs = s.n == 0 ? "" : s.has_temp ? "ok" : "stale";
    return s.has_temp ? s.temp : sta.roomTemperature;
}

// Off-at-setpoint runtime status (shared by /api/settings and /api/park).
cJSON* park_status_json() {
    park_ctl::Snapshot ps = park_ctl::snapshot();
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "enabled", ps.enabled);
    cJSON_AddStringToObject(o, "state", park::state_str(ps.state));
    cJSON_AddBoolToObject(o, "parked", ps.parked);
    cJSON_AddStringToObject(o, "reason", ps.reason.c_str());
    cJSON_AddBoolToObject(o, "unit_on", ps.unit_on);
    cJSON_AddBoolToObject(o, "satisfied", ps.satisfied);
    cJSON_AddBoolToObject(o, "held", ps.held);      // waiting for its time slice
    if (ps.held) {
        // What the other rooms are doing while this one is paused: "heat",
        // "cool", or "switch" (changeover / waiting for a schedule).
        const slice_ctl::Status ss = slice_ctl::status();
        cJSON_AddStringToObject(o, "tandem",
            ss.have_state && ss.state.phase == slice::Phase::Run
                ? slice::mode_str(ss.state.mode) : "switch");
        // Seconds until this room's turn starts: the rest of the pause between
        // modes, or (while it needs a turn) the latest the running turn can
        // end plus that pause. Null when unknown.
        const slice::Mode wants = ss.members.empty() ? slice::Mode::None : ss.members[0].wants;
        bool has_eta = false;
        int64_t eta = 0;
        if (ss.have_state && wants != slice::Mode::None) {
            const int64_t el = ss.elapsed_s, settle = ss.cfg.settle_s;
            // A changeover past its settle time is a stale copy of the
            // coordinator's state (it has already moved on): no estimate.
            if (ss.state.phase == slice::Phase::Changeover && ss.state.mode == wants) {
                has_eta = el < settle;
                eta = settle - el;
            } else if (ss.state.phase == slice::Phase::Run && ss.state.mode != wants &&
                       ps.need == park::Need::Yes) {
                const int64_t decide = ss.state.decide_s ? ss.state.decide_s : ss.cfg.max_slice_s;
                has_eta = true;
                eta = decide - el + settle;
            }
        }
        if (has_eta) {
            cJSON_AddNumberToObject(o, "tandem_eta_s", (double)(eta < 0 ? 0 : eta));
            cJSON_AddStringToObject(o, "tandem_next", slice::mode_str(wants));
        } else {
            cJSON_AddNullToObject(o, "tandem_eta_s");
        }
    }
    cJSON_AddBoolToObject(o, "forced", ps.forced);  // enabled by time-slicing
    cJSON_AddBoolToObject(o, "slicing", ps.slicing);  // group time-slicing is on
    cJSON_AddBoolToObject(o, "quiet_fan", ps.quiet_fan);  // air check at QUIET fan
    if (ps.held) {
        // Tandem need check: does this waiting room need its turn, and is a
        // FAN-mode air check running to find out.
        cJSON_AddStringToObject(o, "need", park::need_str(ps.need));
        cJSON_AddBoolToObject(o, "checking", ps.checking);
    }
    // A fresh room sensor (#142) reads the room: air checks aren't needed.
    cJSON_AddBoolToObject(o, "room_sensor", room_sensor_ctl::active());
    if (ps.next_probe_s >= 0)
        cJSON_AddNumberToObject(o, "next_probe_s", ps.next_probe_s);
    else
        cJSON_AddNullToObject(o, "next_probe_s");
    return o;
}

// ── GET /api/settings ──────────────────────────────────────────────────
esp_err_t handle_get_settings(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_API_AUTH(req);
    cn105::Settings st = s_hooks.get_settings ? s_hooks.get_settings() : cn105::Settings{};
    cn105::Status   sta = s_hooks.get_status ? s_hooks.get_status() : cn105::Status{};

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "power", st.power.c_str());
    cJSON_AddStringToObject(root, "mode", st.mode.c_str());
    cJSON_AddNumberToObject(root, "temperature", st.temperature);
    cJSON_AddStringToObject(root, "fan", st.fan.c_str());
    cJSON_AddStringToObject(root, "vane", st.vane.c_str());
    cJSON_AddStringToObject(root, "wideVane", st.wideVane.c_str());
    cJSON_AddBoolToObject(root, "connected", st.connected);
    cJSON_AddStringToObject(root, "temp_unit",
                            wifi::temp_unit_fahrenheit() ? "F" : "C");
    cJSON_AddStringToObject(root, "lang", wifi::ui_language().c_str());
    // Feature capabilities the UI needs inline (so it can hide the wide-vane
    // control in the same poll it already runs). See /api/capabilities for the
    // full object + control endpoints.
    {
        capability::Snapshot cap = capability::snapshot();
        cJSON* caps = cJSON_CreateObject();
        cJSON* wv = cJSON_CreateObject();
        cJSON_AddStringToObject(wv, "detected",
                                capability::wide_vane_to_str(cap.wideVane));
        cJSON_AddStringToObject(wv, "override",
                                capability::override_to_str(cap.override_m));
        cJSON_AddBoolToObject(wv, "detecting", cap.detecting);
        cJSON_AddBoolToObject(wv, "show", cap.show);
        cJSON_AddItemToObject(caps, "wideVane", wv);
        cJSON_AddItemToObject(root, "caps", caps);
    }
    cJSON_AddNumberToObject(root, "roomTemperature", sta.roomTemperature);
    // Wall sensors (#142) for the dial's room-sensor icon; absent with none paired.
    {
        room_sensor_ctl::Snapshot rs = room_sensor_ctl::snapshot();
        if (!rs.sensors.empty()) {
            cJSON* o = cJSON_AddObjectToObject(root, "roomSensor");
            cJSON_AddStringToObject(o, "state", rs.has_temp ? "ok" : "stale");
            add_num_or_null(o, "temp", rs.has_temp, rs.temp);
            cJSON_AddNumberToObject(o, "n_fresh", rs.n_fresh);
            cJSON* arr = cJSON_AddArrayToObject(o, "sensors");
            for (const auto& v : rs.sensors) {
                cJSON* e = cJSON_CreateObject();
                cJSON_AddStringToObject(e, "name", (v.s.name.empty() ? v.s.id : v.s.name).c_str());
                cJSON_AddBoolToObject(e, "fresh", v.fresh);
                add_num_or_null(e, "temp", v.s.sample.valid, v.s.sample.temp_c);
                add_num_or_null(e, "age_s", v.temp_age_s >= 0, v.temp_age_s);
                cJSON_AddItemToArray(arr, e);
            }
        }
    }
    cJSON_AddBoolToObject(root, "operating", sta.operating);
    cJSON_AddNumberToObject(root, "compressorFrequency", sta.compressorFrequency);
    cJSON_AddItemToObject(root, "park", park_status_json());

    // Extended telemetry — only emitted when the unit actually reported the
    // field, so the UI can render "--" for anything absent.
    if (sta.has_outsideTemp)
        cJSON_AddNumberToObject(root, "outsideTemp", sta.outsideTemp);
    if (sta.has_runtimeHours)
        cJSON_AddNumberToObject(root, "runtimeHours", sta.runtimeHours);
    if (sta.has_inputPowerW)
        cJSON_AddNumberToObject(root, "inputPowerW", sta.inputPowerW);
    if (sta.has_energyKwh)
        cJSON_AddNumberToObject(root, "energyKwh", sta.energyKwh);
    if (sta.has_targetHumidity)
        cJSON_AddNumberToObject(root, "targetHumidity", sta.targetHumidity);
    if (!sta.subMode.empty())
        cJSON_AddStringToObject(root, "subMode", sta.subMode.c_str());
    if (!sta.stage.empty())
        cJSON_AddStringToObject(root, "stage", sta.stage.c_str());
    if (!sta.direction.empty())
        cJSON_AddStringToObject(root, "direction", sta.direction.c_str());
    if (!sta.errorCode.empty())
        cJSON_AddStringToObject(root, "errorCode", sta.errorCode.c_str());

    char* str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, str);
    cJSON_free(str);
    cJSON_Delete(root);
    return ESP_OK;
}

// Helper: read full request body into a heap buffer (caller frees).
char* recv_body(httpd_req_t* req) {
    int total = req->content_len;
    if (total <= 0 || total > 1024) return nullptr;
    char* buf = (char*)malloc(total + 1);
    if (!buf) return nullptr;
    int off = 0;
    while (off < total) {
        int r = httpd_req_recv(req, buf + off, total - off);
        if (r <= 0) { free(buf); return nullptr; }
        off += r;
    }
    buf[total] = '\0';
    return buf;
}

void apply(hvac_mqtt::Command::Kind kind, const char* value) {
    if (!s_hooks.apply_command) return;
    hvac_mqtt::Command cmd{kind, value ? value : ""};
    s_hooks.apply_command(cmd);
}

// ── POST /api/settings ─────────────────────────────────────────────────
esp_err_t handle_post_settings(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_API_AUTH(req);
    char* body = recv_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty or oversized body");
        return ESP_FAIL;
    }
    cJSON* json = cJSON_Parse(body);
    free(body);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    using K = hvac_mqtt::Command::Kind;
    const cJSON* v;
    if ((v = cJSON_GetObjectItem(json, "mode")) && cJSON_IsString(v) &&
        strcasecmp(v->valuestring, "auto") == 0 && slice_ctl::enabled()) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "AUTO is not available in Tandem");
        return ESP_FAIL;
    }
    std::string who = web_actor_name(req);
    const char* wname = who.empty() ? "" : who.c_str();
    const std::string wip = peer_ipv4(req);
    const bool admin = admin_authorized(req);
    // A mode command also powers the head on (HA expresses on/off as a mode),
    // so apply power after it: {"power":"OFF","mode":"COOL"} stores COOL and
    // leaves the head off.
    const cJSON* jmode = cJSON_GetObjectItem(json, "mode");
    const bool has_mode = cJSON_IsString(jmode);
    if (has_mode) {
        apply(K::Mode, jmode->valuestring);
        events::log(events::Cat::Mode, events::Actor::WebUI,
                    ("Set to " + pretty_token(jmode->valuestring)).c_str(), wname, wip);
    }
    if ((v = cJSON_GetObjectItem(json, "power")) && cJSON_IsString(v)) {
        apply(K::Power, v->valuestring);
        // On+mode reads as one "Set to Heat" event; off is always logged.
        bool off = strcasecmp(v->valuestring, "off") == 0;
        if (!has_mode || off)
            events::log(events::Cat::Power, events::Actor::WebUI,
                        off ? "Turned off" : "Turned on", wname, wip);
    }
    if ((v = cJSON_GetObjectItem(json, "fan")) && cJSON_IsString(v)) {
        apply(K::Fan, v->valuestring);
        events::log(events::Cat::Fan, events::Actor::WebUI,
                    ("Fan set to " + pretty_token(v->valuestring)).c_str(), wname, wip);
    }
    if ((v = cJSON_GetObjectItem(json, "vane")) && cJSON_IsString(v))
        apply(K::Vane, v->valuestring);
    if ((v = cJSON_GetObjectItem(json, "wideVane")) && cJSON_IsString(v))
        apply(K::WideVane, v->valuestring);
    if ((v = cJSON_GetObjectItem(json, "temperature")) && cJSON_IsNumber(v)) {
        double t = admin ? v->valuedouble
                         : access_policy::clamp_user_setpoint((float)v->valuedouble);
        char buf[16];
        snprintf(buf, sizeof(buf), "%.1f", t);
        apply(K::Temperature, buf);
        char msg[48];
        snprintf(msg, sizeof(msg), "Target set to %.1f\xC2\xB0""C", t);
        events::log(events::Cat::Setpoint, events::Actor::WebUI, msg, wname, wip);
    }
    if (admin && (v = cJSON_GetObjectItem(json, "remoteTemp")) && cJSON_IsNumber(v)) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.1f", v->valuedouble);
        apply(K::RemoteTemp, buf);
    }
    cJSON_Delete(json);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

// ── GET /api/park ──────────────────────────────────────────────────────
// Off-at-setpoint config + live status.
cJSON* park_config_json(const park::Config& c) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "enabled", c.enabled);
    cJSON_AddNumberToObject(o, "delta_c", c.delta_c);
    cJSON_AddNumberToObject(o, "min_off_s", c.min_off_s);
    cJSON_AddBoolToObject(o, "probe_enabled", c.probe_enabled);
    cJSON_AddNumberToObject(o, "probe_interval_s", c.probe_interval_s);
    cJSON_AddNumberToObject(o, "probe_duration_s", c.probe_duration_s);
    // Grouped: enable / min-off / probe are the outdoor unit's (all its heads).
    cJSON_AddBoolToObject(o, "shared", hvac_group::in_group());
    cJSON_AddItemToObject(o, "status", park_status_json());
    return o;
}

esp_err_t send_park(httpd_req_t* req, const park::Config& c) {
    cJSON* root = park_config_json(c);
    char* str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, str);
    cJSON_free(str);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t handle_park_get(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_API_AUTH(req);
    return send_park(req, park_ctl::config());
}

// ── POST /api/park ─────────────────────────────────────────────────────
// Admin: partial update of the off-at-setpoint tunables. Out-of-range values
// are clamped (see park::sanitize); the response echoes what was applied.
esp_err_t handle_park_post(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_ADMIN(req);
    char* body = recv_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty or oversized body");
        return ESP_FAIL;
    }
    cJSON* json = cJSON_Parse(body);
    free(body);
    if (!json || !cJSON_IsObject(json)) {
        if (json) cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    park::Config c = park_ctl::config();
    const bool was = c.enabled;
    const cJSON* v;
    if ((v = cJSON_GetObjectItem(json, "enabled")) && cJSON_IsBool(v))
        c.enabled = cJSON_IsTrue(v);
    if ((v = cJSON_GetObjectItem(json, "delta_c")) && cJSON_IsNumber(v))
        c.delta_c = (float)v->valuedouble;
    if ((v = cJSON_GetObjectItem(json, "min_off_s")) && cJSON_IsNumber(v) && v->valuedouble >= 0)
        c.min_off_s = (uint32_t)v->valuedouble;
    if ((v = cJSON_GetObjectItem(json, "probe_enabled")) && cJSON_IsBool(v))
        c.probe_enabled = cJSON_IsTrue(v);
    if ((v = cJSON_GetObjectItem(json, "probe_interval_s")) && cJSON_IsNumber(v) && v->valuedouble >= 0)
        c.probe_interval_s = (uint32_t)v->valuedouble;
    if ((v = cJSON_GetObjectItem(json, "probe_duration_s")) && cJSON_IsNumber(v) && v->valuedouble >= 0)
        c.probe_duration_s = (uint32_t)v->valuedouble;
    cJSON_Delete(json);

    park::Config applied = park_ctl::set_config(c);
    if (applied.enabled != was) {
        std::string who = web_actor_name(req);
        events::log(events::Cat::Power, events::Actor::WebUI,
                    applied.enabled ? "Off at setpoint enabled" : "Off at setpoint disabled",
                    who.empty() ? "" : who.c_str(), peer_ipv4(req));
    }
    return send_park(req, applied);
}

// ── GET /api/sensors ───────────────────────────────────────────────────
// Wireless room sensors (#142): the paired sensors and their readings, the
// averaged room temperature, and the Bluetooth / Wi-Fi pairing windows.
esp_err_t send_sensors(httpd_req_t* req) {
    room_sensor_ctl::Snapshot s = room_sensor_ctl::snapshot();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "active", room_sensor_ctl::active());
    add_num_or_null(root, "temp", s.has_temp, s.temp);
    cJSON_AddNumberToObject(root, "n_fresh", s.n_fresh);
    cJSON_AddNumberToObject(root, "max", room_sensor::kMaxSensors);
    cJSON* arr = cJSON_AddArrayToObject(root, "sensors");
    for (const auto& v : s.sensors) {
        const auto& x = v.s;
        cJSON* e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "id", x.id.c_str());
        cJSON_AddStringToObject(e, "kind", room_sensor::kind_str(x.kind));
        cJSON_AddStringToObject(e, "name", x.name.c_str());
        cJSON_AddBoolToObject(e, "fresh", v.fresh);
        add_num_or_null(e, "temp", x.sample.valid, x.sample.temp_c);
        add_num_or_null(e, "humidity", x.sample.has_humidity, x.sample.humidity);
        add_num_or_null(e, "battery", x.sample.has_battery, x.sample.battery);
        add_num_or_null(e, "rssi", x.sample.has_rssi, x.sample.rssi);
        add_num_or_null(e, "temp_age_s", v.temp_age_s >= 0, v.temp_age_s);
        add_num_or_null(e, "seen_age_s", v.seen_age_s >= 0, v.seen_age_s);
        cJSON_AddNumberToObject(e, "stale_after_s", v.stale_s);
        if (x.kind == room_sensor::Kind::ShellyRpc) {
            cJSON_AddStringToObject(e, "host", x.host.c_str());
            cJSON_AddNumberToObject(e, "report_step", x.step_c);
            cJSON_AddBoolToObject(e, "pending", x.sync_pending);
            cJSON_AddStringToObject(e, "sync_error", x.sync_error.c_str());
        }
        cJSON_AddItemToArray(arr, e);
    }
    cJSON* pairing = cJSON_AddObjectToObject(root, "pairing");
    cJSON* pb = cJSON_AddObjectToObject(pairing, "ble");
    cJSON_AddBoolToObject(pb, "open", s.ble_pairing);
    cJSON_AddNumberToObject(pb, "remaining_s", s.ble_remaining_s);
    cJSON* nb = cJSON_AddArrayToObject(pb, "nearby");
    for (size_t i = 0; i < s.nearby.size(); ++i) {
        const auto& n = s.nearby[i];
        cJSON* e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "addr", n.addr.c_str());
        cJSON_AddStringToObject(e, "name", n.name.c_str());
        cJSON_AddNumberToObject(e, "rssi", n.rssi);
        cJSON_AddBoolToObject(e, "encrypted", n.encrypted);
        add_num_or_null(e, "temp", n.has_temp, n.temp_c);
        cJSON_AddNumberToObject(e, "age_s", s.nearby_age_s[i]);
        cJSON_AddItemToArray(nb, e);
    }
    cJSON* pw = cJSON_AddObjectToObject(pairing, "wifi");
    cJSON_AddBoolToObject(pw, "open", s.wifi_pairing);
    cJSON_AddNumberToObject(pw, "remaining_s", s.wifi_remaining_s);
    cJSON* fd = cJSON_AddArrayToObject(pw, "found");
    for (const auto& f : s.found) {
        cJSON* e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", f.name.c_str());
        cJSON_AddStringToObject(e, "ip", f.ip.c_str());
        cJSON_AddItemToArray(fd, e);
    }
    cJSON_AddStringToObject(pw, "state", s.pair_state.c_str());
    cJSON_AddStringToObject(pw, "message", s.pair_message.c_str());
    cJSON* ble = cJSON_AddObjectToObject(root, "ble");
    cJSON_AddBoolToObject(ble, "scanning", s.ble_scanning);
    cJSON_AddStringToObject(ble, "error", s.ble_error.c_str());
    cJSON_AddNumberToObject(ble, "adverts", s.ble_adverts);
    char* str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, str);
    cJSON_free(str);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t handle_sensors_get(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_API_AUTH(req);
    return send_sensors(req);
}

// ── POST /api/sensors ──────────────────────────────────────────────────
// Admin. {"action": ...}:
//   pair_ble              open the 60 s press-the-button Bluetooth window
//   discover              look for awake Shelly Wi-Fi sensors for 3 min
//   pair_wifi {host}      pair the Shelly at host (runs in the background)
//   select {addr}         pair a Bluetooth sensor from the nearby list
//   cancel                close the pairing windows
//   remove {id}           unpair a sensor
//   set {id, name?, report_step?}
// Responds with GET /api/sensors.
esp_err_t handle_sensors_post(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_ADMIN(req);
    char* body = recv_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty or oversized body");
        return ESP_FAIL;
    }
    cJSON* json = cJSON_Parse(body);
    free(body);
    const cJSON* a = json ? cJSON_GetObjectItem(json, "action") : nullptr;
    if (!a || !cJSON_IsString(a)) {
        if (json) cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing action");
        return ESP_FAIL;
    }
    auto str = [&](const char* k, std::string* out) {
        const cJSON* v = cJSON_GetObjectItem(json, k);
        if (!v || !cJSON_IsString(v)) return false;
        *out = v->valuestring;
        return true;
    };
    const std::string action = a->valuestring;
    std::string addr, host, id, name;
    str("addr", &addr);
    str("host", &host);
    str("id", &id);
    const bool has_name = str("name", &name);
    float step = 0.0f;
    const cJSON* rs = cJSON_GetObjectItem(json, "report_step");
    if (rs && cJSON_IsNumber(rs)) step = static_cast<float>(rs->valuedouble);
    cJSON_Delete(json);

    using room_sensor_ctl::Res;
    Res r;
    if (action == "pair_ble") r = room_sensor_ctl::start_ble_pairing();
    else if (action == "discover") r = room_sensor_ctl::start_wifi_pairing();
    else if (action == "pair_wifi") r = room_sensor_ctl::pair_wifi(host);
    else if (action == "select") r = room_sensor_ctl::select(addr);
    else if (action == "cancel") { room_sensor_ctl::cancel_pairing(); r = Res::Ok; }
    else if (action == "remove") r = room_sensor_ctl::remove(id);
    else if (action == "set") {
        if (rs && step <= 0.0f) r = Res::Invalid;
        else r = room_sensor_ctl::configure(id, has_name ? &name : nullptr, step);
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown action");
        return ESP_FAIL;
    }
    switch (r) {
        case Res::Ok: break;
        case Res::Full:
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "This head already has 4 sensors");
            return ESP_FAIL;
        case Res::NotFound:
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND,
                                action == "select" ? "Sensor not heard recently or encrypted"
                                                   : "No such sensor");
            return ESP_FAIL;
        case Res::Invalid:
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid value");
            return ESP_FAIL;
        case Res::Busy:
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Already pairing a sensor");
            return ESP_FAIL;
    }
    return send_sensors(req);
}

// ── GET /api/sensors/report ────────────────────────────────────────────
// Webhook from a paired Shelly Wi-Fi sensor. No login: the per-sensor token
// in the query (k=) is the credential.

esp_err_t handle_sensors_report(httpd_req_t* req) {
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen == 0 || qlen > 256) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad report");
        return ESP_FAIL;
    }
    std::string q(qlen + 1, '\0');
    httpd_req_get_url_query_str(req, &q[0], q.size());
    q.resize(qlen);
    if (!room_sensor_ctl::on_report(q, peer_ipv4(req))) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Unknown sensor");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "ok");
}

// ── GET /api/capabilities ──────────────────────────────────────────────
// Reports detected feature support + user override. Currently wide vane only.
esp_err_t handle_capabilities_get(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_API_AUTH(req);
    capability::Snapshot cap = capability::snapshot();

    cJSON* root = cJSON_CreateObject();
    cJSON* wv = cJSON_CreateObject();
    cJSON_AddStringToObject(wv, "detected", capability::wide_vane_to_str(cap.wideVane));
    cJSON_AddStringToObject(wv, "override", capability::override_to_str(cap.override_m));
    cJSON_AddBoolToObject(wv, "detecting", cap.detecting);
    cJSON_AddBoolToObject(wv, "show", cap.show);
    cJSON_AddItemToObject(root, "wideVane", wv);
    // So the UI can explain why an on-demand detect can't run right now.
    cJSON_AddBoolToObject(root, "unit_connected",
                          s_hooks.unit_connected ? s_hooks.unit_connected() : false);
    bool on = s_hooks.get_settings ? (s_hooks.get_settings().power == "ON") : false;
    cJSON_AddBoolToObject(root, "unit_on", on);

    char* str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, str);
    cJSON_free(str);
    cJSON_Delete(root);
    return ESP_OK;
}

// ── POST /api/capabilities ─────────────────────────────────────────────
// Body: {"wideVane":{"override":"auto|show|hide"}} to set the override, and/or
//       {"wideVane":{"detect":true}} to trigger an on-demand detection run.
esp_err_t handle_capabilities_post(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_ADMIN(req);
    char* body = recv_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty or oversized body");
        return ESP_FAIL;
    }
    cJSON* json = cJSON_Parse(body);
    free(body);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    std::string who = web_actor_name(req);
    const cJSON* wv = cJSON_GetObjectItem(json, "wideVane");
    if (cJSON_IsObject(wv)) {
        const cJSON* ov = cJSON_GetObjectItem(wv, "override");
        if (cJSON_IsString(ov)) {
            capability::Override o = capability::override_from_str(ov->valuestring);
            capability::set_wide_vane_override(o);
            events::log(events::Cat::System, events::Actor::WebUI,
                        (std::string("Wide-vane control set to ") +
                         capability::override_to_str(o)).c_str(),
                        who.empty() ? "" : who.c_str(), peer_ipv4(req));
        }
        const cJSON* det = cJSON_GetObjectItem(wv, "detect");
        if (cJSON_IsTrue(det)) {
            capability::request_wide_vane_detect();
            events::log(events::Cat::System, events::Actor::WebUI,
                        "Wide-vane detection requested",
                        who.empty() ? "" : who.c_str(), peer_ipv4(req));
        }
    }
    cJSON_Delete(json);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

esp_err_t handle_restart(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_ADMIN(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"restarting\"}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;  // unreachable
}

// ── POST /api/system/factory_reset ─────────────────────────────────────
// Full reset: wipe ALL persisted config (Wi-Fi, MQTT, group, name, passwords)
// and reboot into first-run provisioning. See factory_reset::perform().
esp_err_t handle_factory_reset(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_ADMIN(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"reset\"}");
    vTaskDelay(pdMS_TO_TICKS(500));  // flush the response before we wipe + reboot
    factory_reset::perform();        // erases all NVS and restarts; never returns
    return ESP_OK;  // unreachable
}

// ── POST /api/ota — stream a raw .bin upload into the inactive slot ─────
esp_err_t handle_ota_upload(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_ADMIN(req);
    int total = req->content_len;
    if (ota::local_begin(total > 0 ? total : 0) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "OTA busy or no partition");
        return ESP_FAIL;
    }
    char buf[2048];
    int remaining = total;
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf, sizeof(buf) < (size_t)remaining
                                              ? sizeof(buf) : remaining);
        if (r <= 0) {
            ota::local_abort();
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Upload interrupted");
            return ESP_FAIL;
        }
        if (ota::local_write(buf, r) != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
            return ESP_FAIL;
        }
        remaining -= r;
    }
    if (ota::local_end() != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Image validation failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"rebooting\"}");
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;  // unreachable
}

// ── POST /api/ota/url — pull firmware from a URL (background) ───────────
esp_err_t handle_ota_url(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_ADMIN(req);
    char* body = recv_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty or oversized body");
        return ESP_FAIL;
    }
    cJSON* json = cJSON_Parse(body);
    free(body);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    const cJSON* jurl = cJSON_GetObjectItem(json, "url");
    if (!jurl || !cJSON_IsString(jurl) || strlen(jurl->valuestring) == 0) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing url");
        return ESP_FAIL;
    }
    esp_err_t err = ota::start_url(jurl->valuestring);
    cJSON_Delete(json);

    httpd_resp_set_type(req, "application/json");
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"status\":\"busy\"}");
    }
    return httpd_resp_sendstr(req, "{\"status\":\"started\"}");
}

// ── GET /api/ota/status ────────────────────────────────────────────────
esp_err_t handle_ota_status(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_ADMIN(req);
    ota::Status s = ota::get_status();
    const char* state = "idle";
    switch (s.state) {
        case ota::State::InProgress: state = "in_progress"; break;
        case ota::State::Success:    state = "success"; break;
        case ota::State::Failed:     state = "failed"; break;
        default:                     state = "idle"; break;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state", state);
    cJSON_AddNumberToObject(root, "progress", s.progress);
    cJSON_AddStringToObject(root, "message", s.message.c_str());
    cJSON_AddBoolToObject(root, "busy", ota::busy());
    char* str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, str);
    cJSON_free(str);
    cJSON_Delete(root);
    return ESP_OK;
}

// ── GET /api/update — cached GitHub release check result ───────────────
esp_err_t handle_update_get(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_ADMIN(req);
    ota::UpdateInfo u = ota::get_update_info();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "current", u.current_version.c_str());
    cJSON_AddStringToObject(root, "latest", u.latest_version.c_str());
    cJSON_AddBoolToObject(root, "update_available", u.update_available);
    cJSON_AddBoolToObject(root, "checking", u.checking);
    cJSON_AddBoolToObject(root, "checked", u.checked);
    cJSON_AddStringToObject(root, "published_at", u.published_at.c_str());
    cJSON_AddStringToObject(root, "release_url", u.release_url.c_str());
    cJSON_AddStringToObject(root, "error", u.error.c_str());
    cJSON_AddStringToObject(root, "last_trigger", u.last_trigger.c_str());
    cJSON_AddStringToObject(root, "last_requester", u.last_requester.c_str());
    int64_t age_s = u.last_checked_ms > 0
                        ? (esp_timer_get_time() / 1000 - u.last_checked_ms) / 1000
                        : -1;
    cJSON_AddNumberToObject(root, "last_check_age_s", (double)age_s);
    char* str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, str);
    cJSON_free(str);
    cJSON_Delete(root);
    return ESP_OK;
}

// ── POST /api/update/check — trigger an immediate GitHub poll ──────────
// Build an "IP ua=..." string for the calling client so an unexpected check
// (not the dashboard button) can be traced to its source.
std::string requester_str(httpd_req_t* req) {
    char ip[48] = "unknown";
    int fd = httpd_req_to_sockfd(req);
    if (fd >= 0) {
        struct sockaddr_in6 a;
        socklen_t l = sizeof(a);
        if (getpeername(fd, (struct sockaddr*)&a, &l) == 0)
            inet_ntop(AF_INET6, &a.sin6_addr, ip, sizeof(ip));
    }
    char ua[96] = "";
    size_t ualen = httpd_req_get_hdr_value_len(req, "User-Agent");
    if (ualen > 0 && ualen < sizeof(ua))
        httpd_req_get_hdr_value_str(req, "User-Agent", ua, sizeof(ua));
    char buf[160];
    snprintf(buf, sizeof(buf), "%s ua=\"%s\"", ip, ua);
    return buf;
}

esp_err_t handle_update_check(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_ADMIN(req);
    std::string who = requester_str(req);
    ESP_LOGI(TAG, "/api/update/check from %s", who.c_str());
    ota::check_now(who.c_str());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"checking\"}");
}

// ── POST /api/update/install — install the latest GitHub release ───────
esp_err_t handle_update_install(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_ADMIN(req);
    ESP_LOGI(TAG, "/api/update/install from %s", requester_str(req).c_str());
    esp_err_t err = ota::install_latest();
    httpd_resp_set_type(req, "application/json");
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"status\":\"unavailable\"}");
    }
    return httpd_resp_sendstr(req, "{\"status\":\"started\"}");
}

// ── POST /api/diag/reset-brownout — zero the cumulative brownout count ─
esp_err_t handle_diag_reset_brownout(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_ADMIN(req);
    ESP_LOGI(TAG, "/api/diag/reset-brownout from %s", requester_str(req).c_str());
    diag::reset_brownout_count();
    if (s_hooks.get_diag) {
        DiagTelemetry dg = s_hooks.get_diag();
        hvac_mqtt::publish_diag_state({dg.reset_reason, dg.brownout_count,
                                       dg.vin_sag_count, dg.vin_min_mv,
                                       wifi::get_rssi(), wifi::disconnect_count()});
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

// ── POST /api/diag/reset-wifi-drops — zero the session WiFi-drop count ─
esp_err_t handle_diag_reset_wifi_drops(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_ADMIN(req);
    ESP_LOGI(TAG, "/api/diag/reset-wifi-drops from %s", requester_str(req).c_str());
    wifi::reset_disconnect_count();
    if (s_hooks.get_diag) {
        DiagTelemetry dg = s_hooks.get_diag();
        hvac_mqtt::publish_diag_state({dg.reset_reason, dg.brownout_count,
                                       dg.vin_sag_count, dg.vin_min_mv,
                                       wifi::get_rssi(), wifi::disconnect_count()});
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

// ── POST /api/diag/reset-vin-min — clear the persisted all-time-low input ─
esp_err_t handle_diag_reset_vin_min(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_ADMIN(req);
    ESP_LOGI(TAG, "/api/diag/reset-vin-min from %s", requester_str(req).c_str());
    diag::reset_vin_min_ever();
    if (s_hooks.get_diag) {
        DiagTelemetry dg = s_hooks.get_diag();
        hvac_mqtt::publish_diag_state({dg.reset_reason, dg.brownout_count,
                                       dg.vin_sag_count, dg.vin_min_mv,
                                       wifi::get_rssi(), wifi::disconnect_count()});
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

// ── GET /api/mqtt — current broker settings (password masked) ──────────
esp_err_t handle_mqtt_get(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_ADMIN(req);
    hvac_mqtt::StoredSettings s = hvac_mqtt::get_settings();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "host", s.host.c_str());
    cJSON_AddNumberToObject(root, "port", s.port);
    cJSON_AddStringToObject(root, "username", s.username.c_str());
    cJSON_AddStringToObject(root, "base_topic", s.base_topic.c_str());
    cJSON_AddStringToObject(root, "friendly_name", s.friendly_name.c_str());
    cJSON_AddBoolToObject(root, "password_set", !s.password.empty());
    cJSON_AddBoolToObject(root, "connected",
                          s_hooks.mqtt_connected && s_hooks.mqtt_connected());
    char* str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, str);
    cJSON_free(str);
    cJSON_Delete(root);
    return ESP_OK;
}

// ── POST /api/mqtt — save broker settings to NVS, then reboot ───────────
esp_err_t handle_mqtt_post(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_ADMIN(req);
    char* body = recv_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty or oversized body");
        return ESP_FAIL;
    }
    cJSON* json = cJSON_Parse(body);
    free(body);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    // Start from the current settings so omitted fields are preserved (notably
    // the password, which the GET never returns).
    hvac_mqtt::StoredSettings s = hvac_mqtt::get_settings();
    const cJSON* v;
    if ((v = cJSON_GetObjectItem(json, "host")) && cJSON_IsString(v))
        s.host = v->valuestring;
    if ((v = cJSON_GetObjectItem(json, "port")) && cJSON_IsNumber(v))
        s.port = v->valueint;
    if ((v = cJSON_GetObjectItem(json, "username")) && cJSON_IsString(v))
        s.username = v->valuestring;
    if ((v = cJSON_GetObjectItem(json, "password")) && cJSON_IsString(v))
        s.password = v->valuestring;
    if ((v = cJSON_GetObjectItem(json, "base_topic")) && cJSON_IsString(v))
        s.base_topic = v->valuestring;
    if ((v = cJSON_GetObjectItem(json, "friendly_name")) && cJSON_IsString(v))
        s.friendly_name = v->valuestring;
    cJSON_Delete(json);

    if (s.host.empty()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "host is required");
        return ESP_FAIL;
    }
    if (s.port <= 0) s.port = 1883;

    esp_err_t err = hvac_mqtt::save_settings(s);
    httpd_resp_set_type(req, "application/json");
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS write failed");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "{\"status\":\"saved\",\"message\":\"rebooting\"}");
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;  // unreachable
}

// ── POST /api/device — set the web-UI display name (admin only) ─────────
// The display name is also the HA device label (via the MQTT friendly_name
// fallback), so when a broker is configured a rename triggers a quick reboot to
// re-publish discovery with the new label. Unique ids / topics are uid-keyed, so
// nothing is orphaned. Empty string clears it (falls back to "heatlink-<uid>").
esp_err_t handle_device_post(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_ADMIN(req);
    char* body = recv_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty or oversized body");
        return ESP_FAIL;
    }
    cJSON* json = cJSON_Parse(body);
    free(body);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    const cJSON* v = cJSON_GetObjectItem(json, "name");
    esp_err_t err = ESP_OK;
    bool name_changed = false, unit_changed = false;
    const std::string old_name = wifi::device_display_name();
    // Partial update: only touch a field when its key is present, so a
    // temp_unit-only POST doesn't clear the display name (and vice versa).
    if (v && cJSON_IsString(v)) {
        err = wifi::set_display_name(v->valuestring);
        name_changed = (err == ESP_OK);
    }
    const cJSON* vu = cJSON_GetObjectItem(json, "temp_unit");
    if (err == ESP_OK && vu && cJSON_IsString(vu) && vu->valuestring) {
        char c0 = vu->valuestring[0];
        bool f = (c0 == 'F' || c0 == 'f');
        err = wifi::set_temp_unit(f);
        unit_changed = (err == ESP_OK);
    }
    const cJSON* vtx = cJSON_GetObjectItem(json, "tx_power");
    bool tx_changed = false;
    int old_tx = wifi::get_tx_power_dbm();
    if (err == ESP_OK && vtx && cJSON_IsNumber(vtx)) {
        err = wifi::set_tx_power_dbm((int)vtx->valuedouble);
        tx_changed = (err == ESP_OK) && (wifi::get_tx_power_dbm() != old_tx);
    }
    const cJSON* vl = cJSON_GetObjectItem(json, "lang");
    if (err == ESP_OK && vl && cJSON_IsString(vl)) {
        err = wifi::set_ui_language(vl->valuestring);
    }
    cJSON_Delete(json);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS write failed");
        return ESP_FAIL;
    }
    // Propagate this head's (canonical) display name into the group replica so
    // peers learn it via gossip. No-op when standalone or unchanged.
    if (name_changed) hvac_group::note_self_name(wifi::device_display_name());
    if (name_changed) refresh_house_advert();
    if (unit_changed) {
        std::string who = web_actor_name(req);
        std::string m = std::string("Display set to ") +
                        (wifi::temp_unit_fahrenheit() ? "\xC2\xB0""F" : "\xC2\xB0""C");
        events::log(events::Cat::System, events::Actor::WebUI, m.c_str(), who.c_str(), peer_ipv4(req));
    }
    if (tx_changed) {
        std::string who = web_actor_name(req);
        std::string m = std::string("WiFi TX power set to ") +
                        std::to_string(wifi::get_tx_power_dbm()) + " dBm";
        events::log(events::Cat::System, events::Actor::WebUI, m.c_str(), who.c_str(), peer_ipv4(req));
    }
    // Echo the canonical (sanitised) values so the UI can update in place.
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", wifi::device_display_name().c_str());
    cJSON_AddStringToObject(root, "temp_unit",
                            wifi::temp_unit_fahrenheit() ? "F" : "C");
    cJSON_AddNumberToObject(root, "tx_power_dbm", wifi::get_tx_power_dbm());
    cJSON_AddStringToObject(root, "lang", wifi::ui_language().c_str());
    char* str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, str);
    cJSON_free(str);
    cJSON_Delete(root);
    // A rename changes the HA device label, which is only published at MQTT
    // connect. Reboot to re-publish discovery with the new name so HA updates.
    // Only when the value actually changed and a broker is configured — a
    // temp_unit-only edit, or setting the same name, never reboots.
    if (name_changed && wifi::device_display_name() != old_name &&
        !hvac_mqtt::get_settings().host.empty()) {
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    }
    return ESP_OK;
}
// ── Phase 2b: signed peer polling ──────────────────────────────────────
// Each grouped head runs a background task that fetches every enrolled peer's
// live state over a signed GET /api/group/state and caches the derived
// MemberObs. handle_group_get merges fresh cache entries so conflict detection
// works from the whole group's real state — not just this head's self-view.

struct PeerEntry {
    hvac_group::MemberObs obs;
    int64_t  updated_us   = 0;
    bool     incompatible = false;
};

SemaphoreHandle_t             s_peer_mtx = nullptr;
std::map<std::string, PeerEntry> s_peer_cache;

// A cached peer observation is trusted for this long after its last successful
// poll; past it the peer is treated as Unknown (fail-safe → Indeterminate).
constexpr int64_t kPeerFreshUs   = 45LL * 1000 * 1000;  // 45 s
constexpr int     kPollIntervalMs = 12000;              // ~12 s cadence

// Read a request header into a std::string ("" if absent/oversized).
std::string get_header(httpd_req_t* req, const char* name) {
    size_t len = httpd_req_get_hdr_value_len(req, name);
    if (len == 0 || len > 256) return "";
    std::string v(len + 1, '\0');
    if (httpd_req_get_hdr_value_str(req, name, &v[0], len + 1) != ESP_OK) return "";
    v.resize(len);
    return v;
}

// ── GET /api/group/state — signed peer snapshot (Phase 2b) ─────────────
// NOT session-gated: authentication is the HMAC signature over
// signing_string(sender, self, gid, nonce, "") using the shared group secret.
// Only an enrolled peer holding that secret can produce a valid signature.
esp_err_t handle_group_state(httpd_req_t* req) {
    set_cors(req);
    httpd_resp_set_type(req, "application/json");

    if (!hvac_group::in_group()) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_sendstr(req, "{\"error\":\"not in a group\"}");
        return ESP_FAIL;
    }
    hvac_group::GroupConfig g = hvac_group::get();
    std::string sender = get_header(req, "X-Group-Sender");
    std::string gid    = get_header(req, "X-Group-Id");
    std::string nonce  = get_header(req, "X-Group-Nonce");
    std::string sig    = get_header(req, "X-Group-Sig");

    // Authenticate by shared-secret possession (HMAC below), NOT by current
    // membership: a head an admin just removed elsewhere must still be able to
    // poll us once so it learns its own tombstone via the replica and leaves.
    // Gating on is_peer() here would strand the evicted head in the group.
    if (!hvac_group::is_valid_uid(sender) || !hvac_group::is_valid_nonce(nonce) ||
        gid != g.group_id) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_sendstr(req, "{\"error\":\"unauthorized\"}");
        return ESP_FAIL;
    }
    const std::string self = hvac_group::self_uid();
    if (!hvac_group::hmac_verify(
            hvac_group::signing_string(sender, self, gid, nonce, ""), sig)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_sendstr(req, "{\"error\":\"bad signature\"}");
        return ESP_FAIL;
    }

    cn105::Settings st  = s_hooks.get_settings ? s_hooks.get_settings() : cn105::Settings{};
    cn105::Status   sta = s_hooks.get_status   ? s_hooks.get_status()   : cn105::Status{};

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "uid", self.c_str());
    cJSON_AddStringToObject(root, "power", st.power.c_str());
    cJSON_AddStringToObject(root, "mode", st.mode.c_str());
    cJSON_AddBoolToObject(root, "operating", sta.operating);
    cJSON_AddStringToObject(root, "subMode", sta.subMode.c_str());
    cJSON_AddStringToObject(root, "stage", sta.stage.c_str());
    // Display-only extras consumed by the poller's Zones-tab cards (pv-compatible:
    // an older peer simply omits them and the card renders "--").
    {
        const char* rs = "";
        cJSON_AddNumberToObject(root, "roomTemp", display_room_temp(sta, &rs));
        if (*rs) cJSON_AddStringToObject(root, "rs", rs);
    }
    cJSON_AddNumberToObject(root, "targetTemp", st.temperature);
    cJSON_AddNumberToObject(root, "pv", hvac_group::kProtocolVersion);
    // Time-slicing (#133). Additive fields (older peers ignore them); "slice_v"
    // advertises the capability so slicing is only enabled when every member
    // understands it.
    {
        slice_ctl::Publish sp = slice_ctl::publish();
        cJSON_AddNumberToObject(root, "slice_v", slice::kSliceVersion);
        cJSON_AddBoolToObject(root, "sat", sp.satisfied);
        cJSON_AddStringToObject(root, "need", slice::need_str(sp.need));
        if (sp.has_state) {
            cJSON* js = cJSON_CreateObject();
            cJSON_AddNumberToObject(js, "t", (double)sp.state.term);
            cJSON_AddNumberToObject(js, "s", (double)sp.state.seq);
            cJSON_AddStringToObject(js, "m", slice::mode_str(sp.state.mode));
            cJSON_AddStringToObject(js, "p", slice::phase_str(sp.state.phase));
            cJSON_AddNumberToObject(js, "e", sp.state.elapsed_s);
            cJSON_AddStringToObject(js, "c", sp.state.coordinator.c_str());
            if (sp.state.decide_s) cJSON_AddNumberToObject(js, "d", sp.state.decide_s);
            cJSON_AddBoolToObject(js, "k", sp.state.check);
            cJSON_AddItemToObject(root, "slice", js);
        }
    }
    // Admin-password sync: the verifier record, sealed under a group subkey so
    // it never crosses the LAN in the clear. Newer epoch wins on the poller.
    {
        uint8_t rec[AUTH_ADMIN_REC_LEN];
        uint8_t k[32];
        std::string sealed;
        if (auth_mgr_admin_record(rec) && hvac_group::group_subkey("hl-adm1", k) &&
            hvac_group::seal(k, std::string((const char*)rec, sizeof(rec)), sealed)) {
            cJSON* ja = cJSON_CreateObject();
            cJSON_AddNumberToObject(ja, "e", (double)auth_mgr_admin_epoch());
            cJSON_AddStringToObject(ja, "s", sealed.c_str());
            cJSON_AddItemToObject(root, "adm", ja);
        }
        memset(k, 0, sizeof(k));
    }
    // Ride the replicated group state (label + names + membership CRDT) on the
    // snapshot so the poller can anti-entropy-merge it (Phase 6 gossip).
    std::string rep = hvac_group::replica_json();
    if (!rep.empty()) {
        cJSON* rj = cJSON_Parse(rep.c_str());
        if (rj) cJSON_AddItemToObject(root, "replica", rj);
    }
    char* body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"oom\"}");
        return ESP_FAIL;
    }
    // Sign the response so the poller can trust it (bound to this nonce+receiver).
    std::string rsig = hvac_group::hmac_hex(
        hvac_group::signing_string(self, sender, gid, nonce, body));
    httpd_resp_set_hdr(req, "X-Group-Sig", rsig.c_str());
    esp_err_t r = httpd_resp_sendstr(req, body);
    cJSON_free(body);
    return r;
}

// Fetch one peer's signed snapshot; on success fill @p out (state=Known) and set
// @p incompatible on a protocol-version mismatch. Returns false on any failure.

// Captures the peer's signed response (X-Group-Sig header + body). Response
// headers are ONLY visible through the event callback — esp_http_client_get_header
// returns *request* headers, so it can't be used to read the peer's signature.
struct PollCtx {
    std::string sig;
    std::string body;
};

esp_err_t poll_evt(esp_http_client_event_t* evt) {
    auto* c = static_cast<PollCtx*>(evt->user_data);
    if (!c) return ESP_OK;
    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key &&
        strcasecmp(evt->header_key, "X-Group-Sig") == 0) {
        c->sig = evt->header_value ? evt->header_value : "";
    } else if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->data_len > 0) {
        // Body carries the peer snapshot + replicated group-state CRDT, so allow
        // headroom beyond the tiny status fields for the membership/name payload.
        if (c->body.size() < 6144)
            c->body.append(static_cast<const char*>(evt->data), evt->data_len);
    }
    return ESP_OK;
}

// Admin-password gossip. auth_manager isn't thread-safe, so the poll task only
// decrypts the peer's record here and hands it to the httpd thread to compare
// and adopt.
struct AdminAdopt {
    uint8_t     rec[AUTH_ADMIN_REC_LEN];
    uint32_t    epoch;
    std::string from;
};

void adopt_admin_work(void* arg) {
    auto* a = static_cast<AdminAdopt*>(arg);
    uint8_t mine[AUTH_ADMIN_REC_LEN];
    std::string mine_hex;
    if (auth_mgr_admin_record(mine)) mine_hex = hvac_group::to_hex(mine, sizeof(mine));
    const std::string peer_hex = hvac_group::to_hex(a->rec, sizeof(a->rec));
    if (hvac_group::should_adopt_admin(auth_mgr_admin_epoch(), mine_hex, a->epoch, peer_hex) &&
        auth_mgr_adopt_admin(a->rec, a->epoch)) {
        std::string who = hvac_group::member_display_name(a->from);
        events::log(events::Cat::System, events::Actor::Group,
                    "Admin password synced from the group", who.empty() ? a->from : who);
    }
    memset(a->rec, 0, sizeof(a->rec));
    delete a;
}

void note_peer_admin(const std::string& uid, const cJSON* ja) {
    if (!ja || !cJSON_IsObject(ja) || !s_server) return;
    const cJSON* je = cJSON_GetObjectItem(ja, "e");
    const cJSON* js = cJSON_GetObjectItem(ja, "s");
    if (!cJSON_IsNumber(je) || je->valuedouble < 0 || je->valuedouble > 4294967295.0 ||
        !cJSON_IsString(js))
        return;
    const uint32_t epoch = (uint32_t)je->valuedouble;
    // Cheap pre-filter (racy read is fine; the httpd side re-checks).
    if (epoch < auth_mgr_admin_epoch()) return;
    uint8_t k[32];
    std::string plain;
    const bool ok = hvac_group::group_subkey("hl-adm1", k) &&
                    hvac_group::open_sealed(k, js->valuestring, plain);
    memset(k, 0, sizeof(k));
    if (!ok || plain.size() != AUTH_ADMIN_REC_LEN) return;
    auto* a = new (std::nothrow) AdminAdopt;
    if (!a) return;
    memcpy(a->rec, plain.data(), AUTH_ADMIN_REC_LEN);
    std::fill(plain.begin(), plain.end(), '\0');
    a->epoch = epoch;
    a->from  = uid;
    if (httpd_queue_work(s_server, adopt_admin_work, a) != ESP_OK) {
        memset(a->rec, 0, sizeof(a->rec));
        delete a;
    }
}

bool poll_peer(const std::string& uid, hvac_group::MemberObs& out, bool& incompatible) {
    incompatible = false;
    hvac_group::GroupConfig g = hvac_group::get();
    if (g.group_id.empty()) return false;
    const std::string self = hvac_group::self_uid();

    // Resolve the peer's A record from its derived mDNS hostname.
    std::string host = std::string(CONFIG_MDNS_HOSTNAME) + "-" + uid;
    esp_ip4_addr_t addr = {};
    if (mdns_query_a(host.c_str(), 2000, &addr) != ESP_OK) return false;
    uint32_t a = addr.addr;  // network byte order: LSB = first octet
    char ip[16];
    snprintf(ip, sizeof(ip), "%u.%u.%u.%u",
             (unsigned)(a & 0xff), (unsigned)((a >> 8) & 0xff),
             (unsigned)((a >> 16) & 0xff), (unsigned)((a >> 24) & 0xff));

    std::string nonce = hvac_group::generate_nonce();
    std::string sig   = hvac_group::hmac_hex(
        hvac_group::signing_string(self, uid, g.group_id, nonce, ""));
    if (nonce.empty() || sig.empty()) return false;

    PollCtx ctx;
    std::string url = std::string("http://") + ip + "/api/group/state";
    esp_http_client_config_t cfg = {};
    cfg.url           = url.c_str();
    cfg.method        = HTTP_METHOD_GET;
    cfg.timeout_ms    = 3000;
    cfg.event_handler = poll_evt;
    cfg.user_data     = &ctx;
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return false;
    esp_http_client_set_header(cli, "X-Group-Sender", self.c_str());
    esp_http_client_set_header(cli, "X-Group-Id",     g.group_id.c_str());
    esp_http_client_set_header(cli, "X-Group-Nonce",  nonce.c_str());
    esp_http_client_set_header(cli, "X-Group-Sig",    sig.c_str());

    bool ok = false;
    if (esp_http_client_perform(cli) == ESP_OK) {
        int status = esp_http_client_get_status_code(cli);
        const std::string& body = ctx.body;
        if (status == 200 && !ctx.sig.empty() &&
            hvac_group::hmac_verify(
                hvac_group::signing_string(uid, self, g.group_id, nonce, body), ctx.sig)) {
            cJSON* j = cJSON_Parse(body.c_str());
            if (j) {
                const cJSON* juid  = cJSON_GetObjectItem(j, "uid");
                const cJSON* jpwr  = cJSON_GetObjectItem(j, "power");
                const cJSON* jmode = cJSON_GetObjectItem(j, "mode");
                const cJSON* joper = cJSON_GetObjectItem(j, "operating");
                const cJSON* jsub  = cJSON_GetObjectItem(j, "subMode");
                const cJSON* jstg  = cJSON_GetObjectItem(j, "stage");
                const cJSON* jpv   = cJSON_GetObjectItem(j, "pv");
                std::string puid = (juid && cJSON_IsString(juid)) ? juid->valuestring : "";
                if (puid == uid) {
                    int pv = (jpv && cJSON_IsNumber(jpv)) ? jpv->valueint : -1;
                    out = hvac_group::observe(
                        uid, uid,
                        (jpwr  && cJSON_IsString(jpwr))  ? jpwr->valuestring  : "",
                        (jmode && cJSON_IsString(jmode)) ? jmode->valuestring : "",
                        (joper && cJSON_IsBool(joper))   ? cJSON_IsTrue(joper) : false,
                        (jsub  && cJSON_IsString(jsub))  ? jsub->valuestring  : "",
                        (jstg  && cJSON_IsString(jstg))  ? jstg->valuestring  : "");
                    if (pv != hvac_group::kProtocolVersion) {
                        incompatible = true;
                        out.state = hvac_group::MemberState::Incompatible;
                    }
                    // Display-only extras for the Zones-tab cards. observe() has
                    // just overwritten `out`, so fill these after it.
                    const cJSON* jrt = cJSON_GetObjectItem(j, "roomTemp");
                    const cJSON* jtt = cJSON_GetObjectItem(j, "targetTemp");
                    out.room_temp   = (jrt && cJSON_IsNumber(jrt)) ? (float)jrt->valuedouble : -1000.f;
                    out.target_temp = (jtt && cJSON_IsNumber(jtt)) ? (float)jtt->valuedouble : -1000.f;
                    {
                        const cJSON* jrs = cJSON_GetObjectItem(j, "rs");
                        if (cJSON_IsString(jrs) && (!strcmp(jrs->valuestring, "ok") ||
                                                    !strcmp(jrs->valuestring, "stale")))
                            out.room_sensor = jrs->valuestring;
                    }
                    out.ip          = ip;
                    {
                        slice_ctl::PeerObs so;
                        const cJSON* jsv = cJSON_GetObjectItem(j, "slice_v");
                        so.capable = jsv && cJSON_IsNumber(jsv) &&
                                     jsv->valueint == slice::kSliceVersion;
                        so.satisfied = cJSON_IsTrue(cJSON_GetObjectItem(j, "sat"));
                        const cJSON* jneed = cJSON_GetObjectItem(j, "need");
                        if (cJSON_IsString(jneed)) so.need = slice::parse_need(jneed->valuestring);
                        so.power    = (jpwr  && cJSON_IsString(jpwr))  ? jpwr->valuestring  : "";
                        so.mode     = (jmode && cJSON_IsString(jmode)) ? jmode->valuestring : "";
                        so.sub_mode = (jsub  && cJSON_IsString(jsub))  ? jsub->valuestring  : "";
                        const cJSON* js = cJSON_GetObjectItem(j, "slice");
                        if (js && cJSON_IsObject(js)) {
                            const cJSON* jt = cJSON_GetObjectItem(js, "t");
                            const cJSON* jq = cJSON_GetObjectItem(js, "s");
                            const cJSON* jm = cJSON_GetObjectItem(js, "m");
                            const cJSON* jp = cJSON_GetObjectItem(js, "p");
                            const cJSON* je = cJSON_GetObjectItem(js, "e");
                            const cJSON* jc = cJSON_GetObjectItem(js, "c");
                            if (cJSON_IsNumber(jt) && cJSON_IsNumber(jq) && cJSON_IsString(jm) &&
                                cJSON_IsString(jp) && cJSON_IsNumber(je) && cJSON_IsString(jc) &&
                                jt->valuedouble >= 0 && jt->valuedouble <= 9007199254740991.0 &&
                                jq->valuedouble >= 0 && jq->valuedouble <= 9007199254740991.0 &&
                                je->valuedouble >= 0 && je->valuedouble <= 4294967295.0) {
                                so.has_state = true;
                                so.state.term = (uint64_t)jt->valuedouble;
                                so.state.seq  = (uint64_t)jq->valuedouble;
                                so.state.mode = slice::parse_mode(jm->valuestring);
                                so.state.phase = slice::parse_phase(jp->valuestring);
                                so.state.elapsed_s = (uint32_t)je->valuedouble;
                                so.state.coordinator = jc->valuestring;
                                const cJSON* jd = cJSON_GetObjectItem(js, "d");
                                if (cJSON_IsNumber(jd) && jd->valuedouble >= 0 &&
                                    jd->valuedouble <= 4294967295.0)
                                    so.state.decide_s = (uint32_t)jd->valuedouble;
                                so.state.check = cJSON_IsTrue(cJSON_GetObjectItem(js, "k"));
                            }
                        }
                        slice_ctl::observe_peer(uid, so);
                    }
                    {
                        std::string pwr = (jpwr  && cJSON_IsString(jpwr))  ? jpwr->valuestring  : "";
                        std::string md  = (jmode && cJSON_IsString(jmode)) ? jmode->valuestring : "";
                        out.mode_str    = (pwr == "OFF" || pwr.empty()) ? std::string("OFF") : md;
                    }
                    // Anti-entropy: merge the peer's replicated group state (label
                    // + names + membership) into ours. cJSON_PrintUnformatted the
                    // sub-object back to a string for the device-side merge.
                    const cJSON* jrep = cJSON_GetObjectItem(j, "replica");
                    if (jrep) {
                        char* rs = cJSON_PrintUnformatted(jrep);
                        if (rs) { hvac_group::merge_remote_json(rs); cJSON_free(rs); }
                    }
                    note_peer_admin(uid, cJSON_GetObjectItem(j, "adm"));
                    ok = true;
                }
                cJSON_Delete(j);
            }
        }
    }
    esp_http_client_cleanup(cli);
    return ok;
}

// Background task: poll every enrolled peer on a fixed cadence into the cache.
void update_status_led();  // defined below; drives the onboard warning glyph
void update_group_mqtt();  // defined below; mirrors group status to Home Assistant

void group_poll_task(void*) {
    refresh_house_advert();
    // Seed our own display name into the replica once at boot so peers learn it
    // even if it was set before this firmware gained name propagation.
    if (hvac_group::in_group())
        hvac_group::note_self_name(wifi::device_display_name());
    for (;;) {
        if (hvac_group::in_group()) {
            hvac_group::GroupConfig g = hvac_group::get();
            for (const auto& uid : g.peers) {
                hvac_group::MemberObs obs;
                bool incompat = false;
                if (poll_peer(uid, obs, incompat)) {
                    xSemaphoreTake(s_peer_mtx, portMAX_DELAY);
                    s_peer_cache[uid] = PeerEntry{obs, esp_timer_get_time(), incompat};
                    xSemaphoreGive(s_peer_mtx);
                }
            }
            // Drop cache entries for peers that are no longer enrolled.
            xSemaphoreTake(s_peer_mtx, portMAX_DELAY);
            for (auto it = s_peer_cache.begin(); it != s_peer_cache.end();) {
                if (!hvac_group::is_peer(it->first)) it = s_peer_cache.erase(it);
                else ++it;
            }
            xSemaphoreGive(s_peer_mtx);
        }
        update_status_led();  // reflect the current group status on the LED
        update_group_mqtt();  // mirror the current group status to Home Assistant
        refresh_house_advert();  // group/label changes arrive via many paths
        vTaskDelay(pdMS_TO_TICKS(kPollIntervalMs));
    }
}

// Look up a peer's cached observation; returns a Known/Incompatible MemberObs if
// a fresh entry exists, otherwise an Unknown placeholder.
hvac_group::MemberObs cached_peer_obs(const std::string& uid) {
    hvac_group::MemberObs m;
    m.uid   = uid;
    m.name  = uid;
    m.state = hvac_group::MemberState::Unknown;
    if (!s_peer_mtx) return m;
    xSemaphoreTake(s_peer_mtx, portMAX_DELAY);
    auto it = s_peer_cache.find(uid);
    if (it != s_peer_cache.end() &&
        (esp_timer_get_time() - it->second.updated_us) < kPeerFreshUs) {
        m = it->second.obs;
        m.uid = uid;
    }
    xSemaphoreGive(s_peer_mtx);
    // Prefer the replicated display name (converges across heads); fall back to
    // the raw uid when no friendly name has propagated yet.
    std::string nm = hvac_group::member_display_name(uid);
    m.name = nm.empty() ? uid : nm;
    return m;
}

// Build the current group view from live self state + the freshly-polled peer
// cache. Single source of truth shared by the web endpoint (GET /api/group) and
// the onboard status-LED driver, so the two never disagree. When @p out is
// non-null it receives the member list (self at index 0, then each peer).
hvac_group::GroupView compute_group_view(std::vector<hvac_group::MemberObs>* out) {
    cn105::Settings st  = s_hooks.get_settings ? s_hooks.get_settings() : cn105::Settings{};
    cn105::Status   sta = s_hooks.get_status   ? s_hooks.get_status()   : cn105::Status{};

    hvac_group::MemberObs self_obs;
    self_obs.uid        = hvac_group::self_uid();
    self_obs.name       = wifi::device_display_name();
    self_obs.state      = hvac_group::MemberState::SelfKnown;
    self_obs.demand     = hvac_group::classify_demand(st.power, st.mode);
    self_obs.power_on   = (self_obs.demand != hvac_group::Demand::Neutral) ||
                          (st.power == "ON");
    self_obs.active_now = sta.operating;
    // STANDBY (+ IDLE + !operating) is the hardware's "my mode is being blocked"
    // signal — the authoritative conflict indicator (see design doc).
    self_obs.standby    = (sta.subMode == "STANDBY") && (sta.stage == "IDLE") &&
                          !sta.operating;
    {
        const char* rs = "";
        self_obs.room_temp   = display_room_temp(sta, &rs);
        self_obs.room_sensor = rs;
    }
    self_obs.target_temp = st.temperature;
    self_obs.mode_str    = (st.power == "OFF") ? std::string("OFF") : st.mode;
    self_obs.ip          = wifi::get_ip();

    std::vector<hvac_group::MemberObs> members;
    members.push_back(self_obs);
    hvac_group::GroupConfig g = hvac_group::get();
    for (const auto& uid : g.peers) members.push_back(cached_peer_obs(uid));

    hvac_group::GroupView view = hvac_group::evaluate_group(members);
    // Time-slicing (#133) schedules HEAT and COOL in turns, so mixed demands
    // are expected rather than a conflict to resolve.
    if (slice_ctl::enabled() &&
        (view.status == hvac_group::GroupStatus::Conflict ||
         view.status == hvac_group::GroupStatus::PendingConflict)) {
        view.status = hvac_group::GroupStatus::Ok;
        view.locked_mode = hvac_group::Demand::Neutral;
        view.locked_by.clear();
        view.conflicts.clear();
    }
    if (out) *out = std::move(members);
    return view;
}

// Map the live group status onto the onboard warning glyph: red for a real
// conflict, amber when the state can't be confirmed, dark otherwise.
void update_status_led() {
    if (!hvac_group::in_group()) {
        status_led::set(status_led::Level::Off);
        return;
    }
    hvac_group::GroupView v = compute_group_view(nullptr);
    switch (v.status) {
        case hvac_group::GroupStatus::Conflict:
        case hvac_group::GroupStatus::PendingConflict:
            status_led::set(status_led::Level::Alert);
            break;
        case hvac_group::GroupStatus::Indeterminate:
            status_led::set(status_led::Level::Warn);
            break;
        default:
            status_led::set(status_led::Level::Off);
            break;
    }
}

// Mirror the current group status to Home Assistant over MQTT (optional/additive).
// Derived from the same GroupView as the web UI and the LED, so all three agree.
// Publishes only on a change (the retained topic keeps HA in sync across
// reconnects) and no-ops entirely when the MQTT client isn't connected.
void update_group_mqtt() {
    if (!hvac_mqtt::is_connected()) return;

    hvac_mqtt::GroupState gs;
    std::string locked_mode;
    std::string locked_by;
    if (!hvac_group::in_group()) {
        gs.in_group     = false;
        gs.status       = "standalone";
        gs.conflict     = false;
        gs.member_count = 0;
    } else {
        hvac_group::GroupView v = compute_group_view(nullptr);
        hvac_group::GroupConfig g = hvac_group::get();
        gs.in_group  = true;
        gs.status    = hvac_group::status_str(v.status);
        gs.conflict  = (v.status == hvac_group::GroupStatus::Conflict) ||
                       (v.status == hvac_group::GroupStatus::PendingConflict);
        // Only report a lock during an actual conflict. Outside one the compressor
        // is simply running whatever mode everyone agreed on; nobody is "held out",
        // and active_now cycles with the compressor, so surfacing a lock holder in
        // the ok state just flickers a name in Home Assistant. Match the web UI.
        if (gs.conflict && v.locked_mode != hvac_group::Demand::Neutral)
            locked_mode = hvac_group::demand_str(v.locked_mode);
        if (gs.conflict)
            locked_by = v.locked_by;
        gs.locked_mode  = locked_mode.c_str();
        gs.locked_by    = locked_by.c_str();
        gs.member_count = static_cast<int>(g.peers.size()) + 1;
    }

    // Suppress republishing an unchanged state each poll — the broker retains it.
    static std::string s_last_pub;
    std::string sig = std::string(gs.status) + '|' + gs.locked_mode + '|' +
                      gs.locked_by + '|' + (gs.conflict ? "1" : "0") + '|' +
                      std::to_string(gs.member_count);
    if (sig == s_last_pub) return;
    s_last_pub = sig;
    hvac_mqtt::publish_group_state(gs);
}

// ── Phase 3: coordinator-per-op resolution ─────────────────────────────
// A user picks a target mode on one head (the coordinator). That head assigns a
// monotonic op_id, applies the change to itself, and fans the op out to every
// enrolled peer over a signed POST /api/group/sync. Peers apply in op_id order
// and never turn an OFF zone on. A minimum dwell between accepted changes guards
// the shared compressor against rapid oscillation from a bug or bad actor.

// Minimum time between accepted mode/power changes on a single head. Sized to
// sit just under the measured ~21 s idle / ~36 s min-off window — long enough to
// stop oscillation, short enough that a legitimate correction isn't locked out
// for an unreasonable time.
constexpr int64_t kGroupDwellUs = 20LL * 1000 * 1000;  // 20 s

SemaphoreHandle_t s_op_mtx      = nullptr;
bool              s_have_last_op = false;
int64_t           s_last_op_us   = 0;

// Has the dwell window elapsed since the last accepted change? Only actual
// changes advance the timer, so idempotent no-ops never consume the window.
bool dwell_ok() {
    if (!s_op_mtx) return true;
    xSemaphoreTake(s_op_mtx, portMAX_DELAY);
    const bool ok = !s_have_last_op ||
                    (esp_timer_get_time() - s_last_op_us) >= kGroupDwellUs;
    xSemaphoreGive(s_op_mtx);
    return ok;
}
void mark_op() {
    if (!s_op_mtx) return;
    xSemaphoreTake(s_op_mtx, portMAX_DELAY);
    s_have_last_op = true;
    s_last_op_us   = esp_timer_get_time();
    xSemaphoreGive(s_op_mtx);
}

// This head's live refrigerant demand + power, from the CN105 settings.
void self_view(hvac_group::Demand& demand, bool& power_on) {
    cn105::Settings st = s_hooks.get_settings ? s_hooks.get_settings() : cn105::Settings{};
    demand   = hvac_group::classify_demand(st.power, st.mode);
    power_on = (st.power == "ON");
}

// Apply a planned resolution op to this head via the CN105 command hooks.
// Returns true if a change was actually issued.
bool apply_resolve_op(const hvac_group::ResolveOp& op) {
    if (!op.change) return false;
    if (op.turn_off)
        apply(hvac_mqtt::Command::Kind::Power, "OFF");
    else
        apply(hvac_mqtt::Command::Kind::Mode, hvac_group::demand_str(op.set_mode));
    return true;
}

// Plan + apply a resolution to THIS head, honouring the dwell guard. Fills
// @p out_reason with a machine token: applied / aligned / dwell.
bool resolve_self(hvac_group::Demand target, hvac_group::ResolveStrategy strat,
                  std::string& out_reason) {
    hvac_group::Demand demand; bool power_on;
    self_view(demand, power_on);
    hvac_group::ResolveOp op = hvac_group::plan_resolution(power_on, demand, target, strat);
    if (!op.change) { out_reason = "aligned"; return false; }
    if (!dwell_ok()) { out_reason = "dwell"; return false; }
    apply_resolve_op(op);
    mark_op();
    out_reason = "applied";
    return true;
}

// Apply an EXPLICIT mode command to THIS head — the coordinator, i.e. the head
// the user (or Home Assistant) directly acted on. Unlike resolve_self (which is
// conflict *resolution* and deliberately never turns an OFF zone on), this
// honours the user's literal intent: power on and switch to @p target even when
// this head was off. The "never disturb an OFF zone" guard in plan_resolution is
// correct only for PEERS receiving the fanned-out op, not for the origin where
// the change IS the user's request. Fills @p out_reason: applied / aligned /
// dwell. Idempotent (already on and in @p target) is an "aligned" no-op that
// does not consume the dwell window.
bool apply_self_command(hvac_group::Demand target, std::string& out_reason) {
    hvac_group::Demand demand; bool power_on;
    self_view(demand, power_on);
    if (power_on && demand == target) { out_reason = "aligned"; return false; }
    if (!dwell_ok()) { out_reason = "dwell"; return false; }
    // apply(Mode, ...) powers the unit on and sets the mode (see on_mqtt_command).
    apply(hvac_mqtt::Command::Kind::Mode, hvac_group::demand_str(target));
    mark_op();
    out_reason = "applied";
    return true;
}

// Coordinator side: send one signed resolution op to a single peer over
// POST /api/group/sync. @p reqbody is the exact JSON that was signed. Returns a
// machine token: the peer's own reason (applied/aligned/stale/dwell) on a
// verified 200, else a transport token (unreachable / http / badsig / error).
std::string sync_peer(const std::string& uid, const std::string& reqbody) {
    hvac_group::GroupConfig g = hvac_group::get();
    if (g.group_id.empty()) return "error";
    const std::string self = hvac_group::self_uid();

    // Resolve the peer's A record from its derived mDNS hostname.
    std::string host = std::string(CONFIG_MDNS_HOSTNAME) + "-" + uid;
    esp_ip4_addr_t addr = {};
    if (mdns_query_a(host.c_str(), 2000, &addr) != ESP_OK) return "unreachable";
    uint32_t a = addr.addr;
    char ip[16];
    snprintf(ip, sizeof(ip), "%u.%u.%u.%u",
             (unsigned)(a & 0xff), (unsigned)((a >> 8) & 0xff),
             (unsigned)((a >> 16) & 0xff), (unsigned)((a >> 24) & 0xff));

    std::string nonce = hvac_group::generate_nonce();
    std::string sig   = hvac_group::hmac_hex(
        hvac_group::signing_string(self, uid, g.group_id, nonce, reqbody));
    if (nonce.empty() || sig.empty()) return "error";

    PollCtx ctx;
    std::string url = std::string("http://") + ip + "/api/group/sync";
    esp_http_client_config_t cfg = {};
    cfg.url           = url.c_str();
    cfg.method        = HTTP_METHOD_POST;
    cfg.timeout_ms    = 4000;
    cfg.event_handler = poll_evt;
    cfg.user_data     = &ctx;
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return "error";
    esp_http_client_set_header(cli, "Content-Type",   "application/json");
    esp_http_client_set_header(cli, "X-Group-Sender", self.c_str());
    esp_http_client_set_header(cli, "X-Group-Id",     g.group_id.c_str());
    esp_http_client_set_header(cli, "X-Group-Nonce",  nonce.c_str());
    esp_http_client_set_header(cli, "X-Group-Sig",    sig.c_str());
    esp_http_client_set_post_field(cli, reqbody.c_str(), reqbody.size());

    std::string result = "error";
    if (esp_http_client_perform(cli) == ESP_OK) {
        int status = esp_http_client_get_status_code(cli);
        if (status == 200 && !ctx.sig.empty() &&
            hvac_group::hmac_verify(
                hvac_group::signing_string(uid, self, g.group_id, nonce, ctx.body), ctx.sig)) {
            cJSON* j = cJSON_Parse(ctx.body.c_str());
            if (j) {
                const cJSON* jr = cJSON_GetObjectItem(j, "reason");
                result = (jr && cJSON_IsString(jr)) ? jr->valuestring : "applied";
                cJSON_Delete(j);
            } else {
                result = "applied";
            }
        } else if (status == 200) {
            result = "badsig";
        } else {
            result = "http";
        }
    } else {
        result = "unreachable";
    }
    esp_http_client_cleanup(cli);
    return result;
}

// A per-member result counts as success (not "partial") only when the change
// definitely converged: it was applied, was already aligned, or the peer had
// already seen an equal/newer op (idempotent).
bool sync_result_ok(const std::string& r) {
    return r == "applied" || r == "aligned" || r == "stale";
}


// Read-only view for the web UI. Peers are polled in the background (Phase 2b);
// a peer with a fresh signed snapshot appears Known/Incompatible, otherwise it
// is Unknown, which keeps the group Indeterminate (fail-safe) rather than
// asserting "no conflict". A head with no group reports "standalone".
// Time-slicing status for GET /api/group and POST /api/group/slice.
cJSON* slice_status_json() {
    slice_ctl::Status ss = slice_ctl::status();
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "enabled", ss.cfg.enabled);
    cJSON_AddNumberToObject(o, "min_slice_s", ss.cfg.min_slice_s);
    cJSON_AddNumberToObject(o, "max_slice_s", ss.cfg.max_slice_s);
    cJSON_AddNumberToObject(o, "settle_s", ss.cfg.settle_s);
    cJSON_AddBoolToObject(o, "available", ss.all_capable);
    cJSON_AddBoolToObject(o, "active", ss.have_state);
    if (ss.have_state) {
        cJSON_AddStringToObject(o, "mode", slice::mode_str(ss.state.mode));
        cJSON_AddStringToObject(o, "phase", slice::phase_str(ss.state.phase));
        cJSON_AddNumberToObject(o, "elapsed_s", ss.elapsed_s);
        // When the running turn is next reconsidered (elapsed seconds).
        cJSON_AddNumberToObject(o, "decide_s",
            ss.state.decide_s ? ss.state.decide_s : ss.cfg.max_slice_s);
    } else {
        cJSON_AddNullToObject(o, "mode");
        cJSON_AddNullToObject(o, "phase");
        cJSON_AddNullToObject(o, "elapsed_s");
        cJSON_AddNullToObject(o, "decide_s");
    }
    cJSON_AddStringToObject(o, "coordinator", ss.coordinator.c_str());
    cJSON_AddBoolToObject(o, "is_coordinator", ss.is_coordinator);
    cJSON* jm = cJSON_CreateArray();
    for (const auto& m : ss.members) {
        cJSON* e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "uid", m.uid.c_str());
        cJSON_AddBoolToObject(e, "fresh", m.fresh);
        cJSON_AddBoolToObject(e, "capable", m.capable);
        cJSON_AddStringToObject(e, "wants", slice::mode_str(m.wants));
        cJSON_AddStringToObject(e, "need", slice::need_str(m.need));
        cJSON_AddStringToObject(e, "directive", ss.cfg.enabled ? slice::directive_str(m.directive)
                                                              : "free");
        cJSON_AddItemToArray(jm, e);
    }
    cJSON_AddItemToObject(o, "members", jm);
    return o;
}

esp_err_t handle_group_get(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_API_AUTH(req);
    hvac_group::GroupConfig g = hvac_group::get();
    const bool grouped = hvac_group::in_group();

    std::vector<hvac_group::MemberObs> members;
    hvac_group::GroupView view = compute_group_view(&members);
    const hvac_group::MemberObs& self_obs = members[0];

    cJSON* root = cJSON_CreateObject();
    if (admin_authorized(req)) cJSON_AddStringToObject(root, "group_id", g.group_id.c_str());
    cJSON_AddStringToObject(root, "group_label", g.group_label.c_str());
    cJSON_AddBoolToObject(root, "in_group", grouped);
    cJSON_AddNumberToObject(root, "protocol_version", hvac_group::kProtocolVersion);

    cJSON* self = cJSON_CreateObject();
    cJSON_AddStringToObject(self, "uid", self_obs.uid.c_str());
    cJSON_AddStringToObject(self, "name", self_obs.name.c_str());
    cJSON_AddStringToObject(self, "configured_demand", hvac_group::demand_str(self_obs.demand));
    cJSON_AddBoolToObject(self, "operating", self_obs.active_now);
    cJSON_AddBoolToObject(self, "standby", self_obs.standby);
    cJSON_AddStringToObject(self, "mode", self_obs.mode_str.c_str());
    if (self_obs.room_temp   > -900.f) cJSON_AddNumberToObject(self, "roomTemp", self_obs.room_temp);
    if (self_obs.target_temp > -900.f) cJSON_AddNumberToObject(self, "targetTemp", self_obs.target_temp);
    if (!self_obs.room_sensor.empty()) cJSON_AddStringToObject(self, "rs", self_obs.room_sensor.c_str());
    if (!self_obs.ip.empty())          cJSON_AddStringToObject(self, "ip", self_obs.ip.c_str());
    cJSON_AddItemToObject(root, "self", self);

    cJSON* jmembers = cJSON_CreateArray();
    for (size_t i = 1; i < members.size(); ++i) {  // skip self at [0]
        const auto& m = members[i];
        cJSON* jm = cJSON_CreateObject();
        cJSON_AddStringToObject(jm, "uid", m.uid.c_str());
        cJSON_AddStringToObject(jm, "name", m.name.c_str());
        cJSON_AddStringToObject(jm, "state",
            m.state == hvac_group::MemberState::Known        ? "known"
          : m.state == hvac_group::MemberState::Incompatible ? "incompatible"
                                                             : "unknown");
        if (m.state == hvac_group::MemberState::Known) {
            cJSON_AddStringToObject(jm, "configured_demand", hvac_group::demand_str(m.demand));
            if (!m.mode_str.empty())        cJSON_AddStringToObject(jm, "mode", m.mode_str.c_str());
            if (m.room_temp   > -900.f)     cJSON_AddNumberToObject(jm, "roomTemp", m.room_temp);
            if (m.target_temp > -900.f)     cJSON_AddNumberToObject(jm, "targetTemp", m.target_temp);
            if (!m.room_sensor.empty())     cJSON_AddStringToObject(jm, "rs", m.room_sensor.c_str());
        }
        if (!m.ip.empty()) cJSON_AddStringToObject(jm, "ip", m.ip.c_str());
        cJSON_AddItemToArray(jmembers, jm);
    }
    cJSON_AddItemToObject(root, "members", jmembers);
    cJSON_AddNumberToObject(root, "member_count",
                            grouped ? static_cast<int>(g.peers.size()) + 1 : 0);

    cJSON_AddStringToObject(root, "status", hvac_group::status_str(view.status));
    if (view.locked_mode != hvac_group::Demand::Neutral)
        cJSON_AddStringToObject(root, "locked_mode", hvac_group::demand_str(view.locked_mode));
    else
        cJSON_AddNullToObject(root, "locked_mode");
    cJSON_AddStringToObject(root, "locked_by", view.locked_by.c_str());

    cJSON* jconf = cJSON_CreateArray();
    for (const auto& c : view.conflicts) {
        cJSON* jc = cJSON_CreateObject();
        cJSON_AddStringToObject(jc, "uid", c.uid.c_str());
        cJSON_AddStringToObject(jc, "name", c.name.c_str());
        cJSON_AddStringToObject(jc, "wants", hvac_group::demand_str(c.wants));
        cJSON_AddItemToArray(jconf, jc);
    }
    cJSON_AddItemToObject(root, "conflicts", jconf);

    cJSON* junk = cJSON_CreateArray();
    for (const auto& n : view.unknown_members)
        cJSON_AddItemToArray(junk, cJSON_CreateString(n.c_str()));
    cJSON_AddItemToObject(root, "unknown_members", junk);

    cJSON* jwarn = cJSON_CreateArray();
    for (const auto& w : view.warnings)
        cJSON_AddItemToArray(jwarn, cJSON_CreateString(w.c_str()));
    cJSON_AddItemToObject(root, "warnings", jwarn);
    cJSON_AddItemToObject(root, "slice", slice_status_json());

    char* str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, str);
    cJSON_free(str);
    cJSON_Delete(root);
    return ESP_OK;
}

// Send a small JSON error body with an explicit HTTP status line.
static esp_err_t send_json_error(httpd_req_t* req, const char* status,
                                 const char* message) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, status);
    char out[128];
    snprintf(out, sizeof(out), "{\"error\":\"%s\"}", message);
    return httpd_resp_sendstr(req, out);
}

// ── Joining a group with the admin password ─────────────────────────────
// A head joins a group by proving it knows the group's admin password, without
// ever sending it. The target publishes its verifier parameters plus a one-time
// nonce; the joiner derives the verifier hash K from the typed password and
// sends an HMAC(K) proof. The reply (group secret, replica, broker and the admin
// verifier record) is sealed with a key derived from K, so only a head that
// knows the password can read it.

constexpr int     kJoinNonceSlots = 4;
constexpr int64_t kJoinNonceTtlUs = 60LL * 1000000;
constexpr size_t  kJoinReplyMax   = 16384;

struct JoinNonce {
    std::string nonce;
    int64_t     exp_us = 0;
};
JoinNonce s_join_nonces[kJoinNonceSlots];  // httpd handlers are single-threaded

void refresh_house_advert();  // defined below

bool is_hex_len(const std::string& s, size_t len) {
    if (s.size() != len) return false;
    for (char c : s)
        if (!isxdigit((unsigned char)c)) return false;
    return true;
}

void join_nonce_put(const std::string& n) {
    int slot = 0;
    for (int i = 1; i < kJoinNonceSlots; ++i)
        if (s_join_nonces[i].exp_us < s_join_nonces[slot].exp_us) slot = i;
    s_join_nonces[slot].nonce  = n;
    s_join_nonces[slot].exp_us = esp_timer_get_time() + kJoinNonceTtlUs;
}

// One-shot: true (and the slot is cleared) if @p n was issued and is unexpired.
bool join_nonce_take(const std::string& n) {
    const int64_t now = esp_timer_get_time();
    for (auto& e : s_join_nonces) {
        if (!e.nonce.empty() && e.nonce == n) {
            const bool live = e.exp_us > now;
            e.nonce.clear();
            e.exp_us = 0;
            return live;
        }
    }
    return false;
}

std::string hmac_key_hex(const uint8_t key[32], const std::string& msg) {
    uint8_t mac[32];
    if (!hvac_group::hmac_sha256(key, 32, msg, mac)) return "";
    return hvac_group::to_hex(mac, sizeof(mac));
}

bool ct_equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    uint8_t d = 0;
    for (size_t i = 0; i < a.size(); ++i) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

// ── GET /api/group/join/params ─ verifier parameters + nonce (NO auth) ──
esp_err_t handle_group_join_params(httpd_req_t* req) {
    set_cors(req);
    int ver = 0;
    uint32_t iters = 0;
    uint8_t salt[AUTH_SALT_LEN];
    uint8_t key[32];
    if (!auth_mgr_admin_verifier(&ver, salt, &iters, key))
        return send_json_error(req, "409 Conflict", "no_admin_password");
    memset(key, 0, sizeof(key));
    std::string nonce = hvac_group::generate_group_id();
    if (nonce.empty()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "rng failed");
        return ESP_FAIL;
    }
    join_nonce_put(nonce);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "ver", ver);
    cJSON_AddNumberToObject(root, "iters", (double)iters);
    cJSON_AddStringToObject(root, "salt", hvac_group::to_hex(salt, sizeof(salt)).c_str());
    cJSON_AddStringToObject(root, "nonce", nonce.c_str());
    cJSON_AddStringToObject(root, "uid", hvac_group::self_uid().c_str());
    cJSON_AddStringToObject(root, "name", wifi::device_display_name().c_str());
    if (hvac_group::in_group())
        cJSON_AddStringToObject(root, "glabel", hvac_group::get().group_label.c_str());
    char* str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, str ? str : "{}");
    cJSON_free(str);
    return ESP_OK;
}

// ── POST /api/group/join/claim ─ joiner proves the password (NO auth) ───
// The HMAC proof IS the authenticator (like /api/login), so this bypasses the
// session gate and counts failures toward the login throttle.
esp_err_t handle_group_join_claim(httpd_req_t* req) {
    set_cors(req);
    char* body = recv_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty or oversized body");
        return ESP_FAIL;
    }
    cJSON* json = cJSON_Parse(body);
    free(body);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    auto str_of = [&](const char* k) {
        const cJSON* v = cJSON_GetObjectItem(json, k);
        return (v && cJSON_IsString(v)) ? std::string(v->valuestring) : std::string();
    };
    std::string joiner = str_of("joiner_uid");
    std::string jn     = str_of("joiner_name");
    std::string nonce  = str_of("nonce");
    std::string cnonce = str_of("cnonce");
    std::string proof  = str_of("proof");
    cJSON_Delete(json);
    if (jn.size() > 48) jn.resize(48);  // bound the self-reported name
    if (!hvac_group::is_valid_uid(joiner) || !is_hex_len(nonce, 32) ||
        !is_hex_len(cnonce, 32) || !is_hex_len(proof, 64))
        return send_json_error(req, "400 Bad Request", "bad request");
    if (joiner == hvac_group::self_uid())
        return send_json_error(req, "400 Bad Request", "cannot join self");
    if (auth_mgr_login_lockout_secs() > 0)
        return send_json_error(req, "429 Too Many Requests", "locked out");
    if (!join_nonce_take(nonce))
        return send_json_error(req, "410 Gone", "nonce expired");

    int ver = 0;
    uint32_t iters = 0;
    uint8_t salt[AUTH_SALT_LEN];
    uint8_t key[32];
    if (!auth_mgr_admin_verifier(&ver, salt, &iters, key))
        return send_json_error(req, "409 Conflict", "no_admin_password");
    for (auto& c : proof) c = (char)tolower((unsigned char)c);
    std::string expect = hmac_key_hex(key, "hl-join1|" + nonce + "|" + cnonce + "|" + joiner);
    if (expect.empty() || !ct_equal(expect, proof)) {
        memset(key, 0, sizeof(key));
        auth_mgr_note_failed_login();
        return send_json_error(req, "401 Unauthorized", "bad password");
    }
    auth_mgr_note_good_login();
    uint8_t ek[32];
    const bool ek_ok = hvac_group::hmac_sha256(key, 32, "hl-enc1|" + nonce + "|" + cnonce, ek);
    memset(key, 0, sizeof(key));
    uint8_t rec[AUTH_ADMIN_REC_LEN];
    if (!ek_ok || !auth_mgr_admin_record(rec)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "crypto failed");
        return ESP_FAIL;
    }

    hvac_group::AdmitOutcome oc;
    esp_err_t aerr = hvac_group::admit_joiner(joiner, jn, wifi::device_display_name(), oc);
    if (aerr == ESP_ERR_INVALID_ARG)
        return send_json_error(req, "400 Bad Request", "bad joiner");
    if (aerr != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "could not admit");
        return ESP_FAIL;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "group_id", oc.group_id.c_str());
    cJSON_AddStringToObject(root, "group_label", oc.group_label.c_str());
    cJSON_AddStringToObject(root, "group_secret", oc.group_secret.c_str());
    cJSON* members = cJSON_CreateArray();
    for (const auto& uid : oc.members)
        cJSON_AddItemToArray(members, cJSON_CreateString(uid.c_str()));
    cJSON_AddItemToObject(root, "members", members);
    if (!oc.replica_json.empty()) {
        cJSON* rj = cJSON_Parse(oc.replica_json.c_str());
        if (rj) cJSON_AddItemToObject(root, "replica", rj);
    }
    // Share the group's broker so a fresh head inherits Home Assistant / MQTT
    // config; the joiner adopts it only if it has none. friendly_name is
    // omitted so each head keeps its own MQTT node name.
    {
        hvac_mqtt::StoredSettings bs = hvac_mqtt::get_settings();
        if (!bs.host.empty()) {
            cJSON* b = cJSON_CreateObject();
            cJSON_AddStringToObject(b, "host", bs.host.c_str());
            cJSON_AddNumberToObject(b, "port", bs.port);
            cJSON_AddStringToObject(b, "username", bs.username.c_str());
            cJSON_AddStringToObject(b, "password", bs.password.c_str());
            cJSON_AddStringToObject(b, "base_topic", bs.base_topic.c_str());
            cJSON_AddItemToObject(root, "broker", b);
        }
    }
    {
        cJSON* adm = cJSON_CreateObject();
        cJSON_AddNumberToObject(adm, "e", (double)auth_mgr_admin_epoch());
        cJSON_AddStringToObject(adm, "r", hvac_group::to_hex(rec, sizeof(rec)).c_str());
        cJSON_AddItemToObject(root, "adm", adm);
    }
    char* plain = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    std::string sealed;
    const bool sealed_ok = plain && hvac_group::seal(ek, plain, sealed);
    if (plain) {
        memset(plain, 0, strlen(plain));
        cJSON_free(plain);
    }
    memset(ek, 0, sizeof(ek));
    if (!sealed_ok) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "seal failed");
        return ESP_FAIL;
    }
    events::log(events::Cat::Membership, events::Actor::System, "Zone joined the group",
                jn.empty() ? joiner : jn, peer_ipv4(req));
    refresh_house_advert();
    std::string out = "{\"sealed\":\"" + sealed + "\"}";
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, out.c_str());
}

// Outbound request to another head (no redirects). Returns the HTTP status,
// or 0 if the head couldn't be reached.
int join_http(const std::string& url, const std::string* post, std::string& resp) {
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.method = post ? HTTP_METHOD_POST : HTTP_METHOD_GET;
    cfg.timeout_ms = 8000;
    cfg.disable_auto_redirect = true;  // never follow a redirect off the chosen host
    cfg.max_redirection_count = 0;
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return 0;
    const int wlen = post ? (int)post->size() : 0;
    if (post) esp_http_client_set_header(cli, "Content-Type", "application/json");
    int status = 0;
    if (esp_http_client_open(cli, wlen) == ESP_OK) {
        if (post) esp_http_client_write(cli, post->data(), wlen);
        esp_http_client_fetch_headers(cli);
        status = esp_http_client_get_status_code(cli);
        char rbuf[512];
        while (resp.size() < kJoinReplyMax) {
            int r = esp_http_client_read(cli, rbuf, sizeof(rbuf));
            if (r <= 0) break;
            resp.append(rbuf, r);
        }
        esp_http_client_close(cli);
    }
    esp_http_client_cleanup(cli);
    return status;
}

bool valid_join_host(const std::string& h) {
    if (h.empty() || h.size() > 64) return false;
    for (char c : h)
        if (!isalnum((unsigned char)c) && c != '.' && c != '-' && c != ':') return false;
    return true;
}

// ── POST /api/group/join ─ this head joins another head's group ────────
// Body: {host, password, token?}. Admin, or not signed in during setup (claim
// token) / the reset window. Proves the password to @c host, adopts the group
// and its admin password, and signs the browser in as admin.
esp_err_t handle_group_join(httpd_req_t* req) {
    set_cors(req);
    char* body = recv_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty or oversized body");
        return ESP_FAIL;
    }
    cJSON* json = cJSON_Parse(body);
    free(body);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    auto str_of = [&](const char* k) {
        const cJSON* v = cJSON_GetObjectItem(json, k);
        return (v && cJSON_IsString(v)) ? std::string(v->valuestring) : std::string();
    };
    std::string host  = str_of("host");
    std::string pw    = str_of("password");
    std::string token = str_of("token");
    cJSON_Delete(json);
    const bool was_admin = admin_authorized(req);
    if (!was_admin && !auth_mgr_join_authorized(token.c_str())) {
        send_unauthorized(req);
        return ESP_FAIL;
    }
    if (hvac_group::in_group())
        return send_json_error(req, "409 Conflict",
                               "This head is already in a group. Leave it first.");
    if (!valid_join_host(host) || pw.empty() || pw.size() > 64)
        return send_json_error(req, "400 Bad Request", "Pick a head and enter the admin password.");

    // 1. Verifier parameters + nonce.
    std::string presp;
    int status = join_http("http://" + host + "/api/group/join/params", nullptr, presp);
    if (status == 0)
        return send_json_error(req, "502 Bad Gateway",
                               "Couldn't reach that head. Check it's powered on and try again.");
    if (status == 404)
        return send_json_error(req, "400 Bad Request",
                               "That head needs a firmware update before it can be joined.");
    if (status == 409)
        return send_json_error(req, "400 Bad Request",
                               "That head has no admin password yet. Set one on it first.");
    cJSON* pj = (status == 200) ? cJSON_Parse(presp.c_str()) : nullptr;
    if (!pj) return send_json_error(req, "502 Bad Gateway", "That head sent a bad reply.");
    const cJSON* jver = cJSON_GetObjectItem(pj, "ver");
    const cJSON* jit  = cJSON_GetObjectItem(pj, "iters");
    const cJSON* jslt = cJSON_GetObjectItem(pj, "salt");
    const cJSON* jnon = cJSON_GetObjectItem(pj, "nonce");
    int ver = cJSON_IsNumber(jver) ? jver->valueint : 0;
    double itd = cJSON_IsNumber(jit) ? jit->valuedouble : -1;
    std::string salt_hex = cJSON_IsString(jslt) ? jslt->valuestring : "";
    std::string nonce    = cJSON_IsString(jnon) ? jnon->valuestring : "";
    cJSON_Delete(pj);
    std::vector<uint8_t> salt;
    if (itd < 0 || itd > AUTH_MAX_ITERS || !is_hex_len(salt_hex, AUTH_SALT_LEN * 2) ||
        !hvac_group::from_hex(salt_hex, salt) || !is_hex_len(nonce, 32))
        return send_json_error(req, "502 Bad Gateway", "That head sent a bad reply.");

    // 2. Prove the password.
    uint8_t key[32];
    if (!auth_mgr_derive_key(pw.c_str(), ver, salt.data(), (uint32_t)itd, key))
        return send_json_error(req, "400 Bad Request",
                               "That head needs a firmware update before it can be joined.");
    std::fill(pw.begin(), pw.end(), '\0');
    const std::string self = hvac_group::self_uid();
    std::string cnonce = hvac_group::generate_group_id();
    std::string proof  = hmac_key_hex(key, "hl-join1|" + nonce + "|" + cnonce + "|" + self);
    uint8_t ek[32];
    const bool ek_ok = hvac_group::hmac_sha256(key, 32, "hl-enc1|" + nonce + "|" + cnonce, ek);
    memset(key, 0, sizeof(key));
    if (cnonce.empty() || proof.empty() || !ek_ok) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "crypto failed");
        return ESP_FAIL;
    }
    std::string creq;
    {
        cJSON* c = cJSON_CreateObject();
        cJSON_AddStringToObject(c, "joiner_uid", self.c_str());
        cJSON_AddStringToObject(c, "joiner_name", wifi::device_display_name().c_str());
        cJSON_AddStringToObject(c, "nonce", nonce.c_str());
        cJSON_AddStringToObject(c, "cnonce", cnonce.c_str());
        cJSON_AddStringToObject(c, "proof", proof.c_str());
        char* cs = cJSON_PrintUnformatted(c);
        cJSON_Delete(c);
        if (cs) { creq = cs; cJSON_free(cs); }
    }
    std::string cresp;
    status = join_http("http://" + host + "/api/group/join/claim", &creq, cresp);
    if (status != 200) {
        memset(ek, 0, sizeof(ek));
        const char* msg;
        switch (status) {
            case 0:   msg = "Couldn't reach that head. Check it's powered on and try again."; break;
            case 401: msg = "Incorrect admin password."; break;
            case 429: msg = "Too many wrong passwords. Wait a few minutes and try again."; break;
            case 410: msg = "That took too long. Please try again."; break;
            case 409: msg = "That head has no admin password yet. Set one on it first."; break;
            default:  msg = "That head didn't accept the request. Please try again."; break;
        }
        return send_json_error(req, "400 Bad Request", msg);
    }

    // 3. Open the sealed group bundle.
    std::string plain;
    {
        cJSON* sj = cJSON_Parse(cresp.c_str());
        const cJSON* js = sj ? cJSON_GetObjectItem(sj, "sealed") : nullptr;
        const bool ok = js && cJSON_IsString(js) &&
                        hvac_group::open_sealed(ek, js->valuestring, plain);
        cJSON_Delete(sj);
        memset(ek, 0, sizeof(ek));
        if (!ok)
            return send_json_error(req, "502 Bad Gateway",
                                   "That head's reply couldn't be verified.");
    }
    cJSON* rj = cJSON_Parse(plain.c_str());
    std::fill(plain.begin(), plain.end(), '\0');
    if (!rj) return send_json_error(req, "502 Bad Gateway", "That head sent a bad reply.");
    const cJSON* gid    = cJSON_GetObjectItem(rj, "group_id");
    const cJSON* glabel = cJSON_GetObjectItem(rj, "group_label");
    const cJSON* gsec   = cJSON_GetObjectItem(rj, "group_secret");
    const cJSON* gmem   = cJSON_GetObjectItem(rj, "members");
    std::vector<std::string> members;
    if (gmem && cJSON_IsArray(gmem)) {
        cJSON* it = nullptr;
        cJSON_ArrayForEach(it, gmem) {
            // Bound the adopted membership — 4 heads share one compressor.
            if (members.size() >= 8) break;
            if (cJSON_IsString(it)) members.push_back(it->valuestring);
        }
    }
    std::string group_id = cJSON_IsString(gid) ? gid->valuestring : "";
    std::string label    = cJSON_IsString(glabel) ? glabel->valuestring : "";
    std::string secret   = cJSON_IsString(gsec) ? gsec->valuestring : "";
    std::string replica_json;
    if (const cJSON* jrep = cJSON_GetObjectItem(rj, "replica")) {
        char* rs = cJSON_PrintUnformatted(jrep);
        if (rs) { replica_json = rs; cJSON_free(rs); }
    }
    // The group's admin password replaces ours.
    std::vector<uint8_t> adm_rec;
    uint32_t adm_epoch = 0;
    if (const cJSON* ja = cJSON_GetObjectItem(rj, "adm")) {
        const cJSON* je = cJSON_GetObjectItem(ja, "e");
        const cJSON* jr = cJSON_GetObjectItem(ja, "r");
        if (cJSON_IsNumber(je) && je->valuedouble >= 0 && je->valuedouble <= 4294967295.0 &&
            cJSON_IsString(jr) && hvac_group::from_hex(jr->valuestring, adm_rec) &&
            adm_rec.size() == AUTH_ADMIN_REC_LEN)
            adm_epoch = (uint32_t)je->valuedouble;
        else
            adm_rec.clear();
    }
    // Inherit the group's broker when we don't have one yet; an already
    // configured head keeps its own.
    bool inherit_broker = false;
    hvac_mqtt::StoredSettings inb;
    if (const cJSON* jb = cJSON_GetObjectItem(rj, "broker")) {
        const cJSON* bh = cJSON_GetObjectItem(jb, "host");
        if (bh && cJSON_IsString(bh) && bh->valuestring[0] &&
            hvac_mqtt::get_settings().host.empty()) {
            inb = hvac_mqtt::get_settings();  // keep our own friendly_name
            inb.host = bh->valuestring;
            const cJSON* x;
            if ((x = cJSON_GetObjectItem(jb, "port")) && cJSON_IsNumber(x)) inb.port = x->valueint;
            if ((x = cJSON_GetObjectItem(jb, "username")) && cJSON_IsString(x)) inb.username = x->valuestring;
            if ((x = cJSON_GetObjectItem(jb, "password")) && cJSON_IsString(x)) inb.password = x->valuestring;
            if ((x = cJSON_GetObjectItem(jb, "base_topic")) && cJSON_IsString(x)) inb.base_topic = x->valuestring;
            if (inb.port <= 0) inb.port = 1883;
            inherit_broker = true;
        }
    }
    cJSON_Delete(rj);
    if (adm_rec.empty())
        return send_json_error(req, "502 Bad Gateway", "That head sent a bad reply.");

    if (hvac_group::join_group(group_id, label, secret, members, replica_json) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "could not adopt group");
        return ESP_FAIL;
    }
    hvac_group::note_self_name(wifi::device_display_name());
    refresh_house_advert();
    auth_mgr_adopt_admin(adm_rec.data(), adm_epoch);
    // Adopting may have logged everyone out; sign this browser in as admin.
    char session[AUTH_SESSION_TOKEN_LEN + 1];
    if (auth_mgr_create_admin_session(session)) set_session_cookie(req, session);

    bool rebooting = false;
    if (inherit_broker && hvac_mqtt::save_settings(inb) == ESP_OK) rebooting = true;

    hvac_group::GroupConfig g = hvac_group::get();
    {
        std::string msg = g.group_label.empty()
                              ? std::string("Joined the group")
                              : std::string("Joined group \"") + g.group_label + "\"";
        events::log(events::Cat::Membership, events::Actor::WebUI, msg,
                    was_admin ? web_actor_name(req) : std::string("admin"), peer_ipv4(req));
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "joined");
    cJSON_AddStringToObject(root, "group_id", g.group_id.c_str());
    cJSON_AddStringToObject(root, "group_label", g.group_label.c_str());
    cJSON_AddNumberToObject(root, "member_count", static_cast<int>(g.peers.size()) + 1);
    cJSON_AddBoolToObject(root, "broker_inherited", rebooting);
    cJSON_AddBoolToObject(root, "rebooting", rebooting);
    char* str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, str ? str : "{}");
    cJSON_free(str);
    if (rebooting) {
        vTaskDelay(pdMS_TO_TICKS(800));
        esp_restart();
    }
    return ESP_OK;
}

// ── POST /api/group/leave ─ drop out of the group (admin) ──────────────
esp_err_t handle_group_leave(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_ADMIN(req);
    std::string who = web_actor_name(req);
    if (hvac_group::leave_group() != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS write failed");
        return ESP_FAIL;
    }
    events::log(events::Cat::Membership, events::Actor::WebUI, "Left the group",
                who.c_str(), peer_ipv4(req));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"left\"}");
}

// ── POST /api/group/sync ─ apply an ordered resolution op (signed peer) ─
// NOT session-gated: authenticated by the HMAC signature over
// signing_string(sender, self, gid, nonce, body). Body:
//   {"op_id":N,"target_mode":"HEAT|COOL","strategy":"flip|off_conflicting"}
// Applies only this head's mode/power (never OTA/settings/wifi), in op_id order,
// honouring the dwell guard. The response is signed back to the coordinator.
esp_err_t handle_group_sync(httpd_req_t* req) {
    set_cors(req);
    httpd_resp_set_type(req, "application/json");

    if (slice_ctl::enabled()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "{\"error\":\"Tandem is enabled\"}");
        return ESP_FAIL;
    }

    if (!hvac_group::in_group()) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_sendstr(req, "{\"error\":\"not in a group\"}");
        return ESP_FAIL;
    }
    hvac_group::GroupConfig g = hvac_group::get();
    std::string sender = get_header(req, "X-Group-Sender");
    std::string gid    = get_header(req, "X-Group-Id");
    std::string nonce  = get_header(req, "X-Group-Nonce");
    std::string sig    = get_header(req, "X-Group-Sig");

    char* raw = recv_body(req);
    std::string body = raw ? raw : "";
    if (raw) free(raw);

    if (!hvac_group::is_valid_uid(sender) || !hvac_group::is_valid_nonce(nonce) ||
        gid != g.group_id || !hvac_group::is_peer(sender) || body.empty()) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_sendstr(req, "{\"error\":\"unauthorized\"}");
        return ESP_FAIL;
    }
    const std::string self = hvac_group::self_uid();
    if (!hvac_group::hmac_verify(
            hvac_group::signing_string(sender, self, gid, nonce, body), sig)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_sendstr(req, "{\"error\":\"bad signature\"}");
        return ESP_FAIL;
    }

    cJSON* j = cJSON_Parse(body.c_str());
    if (!j) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"invalid json\"}");
        return ESP_FAIL;
    }
    const cJSON* jop  = cJSON_GetObjectItem(j, "op_id");
    const cJSON* jtm  = cJSON_GetObjectItem(j, "target_mode");
    const cJSON* jst  = cJSON_GetObjectItem(j, "strategy");
    uint64_t op_id = (jop && cJSON_IsNumber(jop) && jop->valuedouble > 0)
                         ? (uint64_t)jop->valuedouble : 0;
    std::string tm  = (jtm && cJSON_IsString(jtm)) ? jtm->valuestring : "";
    std::string stg = (jst && cJSON_IsString(jst)) ? jst->valuestring : "";
    cJSON_Delete(j);

    hvac_group::Demand target = hvac_group::parse_target_mode(tm);
    if (op_id == 0 || (target != hvac_group::Demand::Heat &&
                       target != hvac_group::Demand::Cool)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"bad op\"}");
        return ESP_FAIL;
    }
    hvac_group::ResolveStrategy strat = hvac_group::parse_strategy(stg);

    // Order + replay guard: only a strictly-newer op is honoured.
    std::string reason;
    if (!hvac_group::accept_op_id(op_id)) {
        reason = "stale";
    } else {
        resolve_self(target, strat, reason);  // applied / aligned / dwell
        if (reason == "applied") {
            std::string sname = hvac_group::member_display_name(sender);
            if (sname.empty()) sname = sender;
            std::string msg = (strat == hvac_group::ResolveStrategy::OffConflicting)
                                  ? "Turned off (group)"
                                  : std::string("Changed to ") +
                                        pretty_token(hvac_group::demand_str(target)) +
                                        " (group)";
            events::log(events::Cat::Group, events::Actor::Group, msg.c_str(),
                        sname.c_str());
        }
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "uid", self.c_str());
    cJSON_AddNumberToObject(root, "op_id", (double)op_id);
    cJSON_AddBoolToObject(root, "applied", reason == "applied");
    cJSON_AddStringToObject(root, "reason", reason.c_str());
    cJSON_AddNumberToObject(root, "pv", hvac_group::kProtocolVersion);
    char* rbody = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!rbody) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"oom\"}");
        return ESP_FAIL;
    }
    std::string rsig = hvac_group::hmac_hex(
        hvac_group::signing_string(self, sender, gid, nonce, rbody));
    httpd_resp_set_hdr(req, "X-Group-Sig", rsig.c_str());
    esp_err_t r = httpd_resp_sendstr(req, rbody);
    cJSON_free(rbody);
    return r;
}

// Coordinate the whole group onto `target` (HEAT/COOL): apply the resolution to
// this head, then send the same signed, op_id-ordered op to every enrolled peer
// via /api/group/sync. When @p results is non-null, a {uid,result} object is
// appended per peer; when @p self_reason_out is non-null it receives this head's
// own result token. Returns true iff every member converged (self + all peers).
// Shared by the /api/group/resolve HTTP handler and the MQTT (Home Assistant)
// mode-command path so both coordinate identically.
bool coordinate_group_to(hvac_group::Demand target, hvac_group::ResolveStrategy strat,
                         cJSON* results, std::string* self_reason_out) {
    hvac_group::GroupConfig g = hvac_group::get();
    uint64_t op_id = hvac_group::issue_op_id();

    // Apply to self first (we are the origin, so no accept_op_id needed). The
    // origin is the head the user/HA explicitly commanded to `target`, so honour
    // it literally — power on and switch — rather than running conflict
    // resolution (which would leave an OFF origin off). Peers below still go
    // through plan_resolution and are never force-powered-on.
    std::string self_reason;
    apply_self_command(target, self_reason);
    if (self_reason_out) *self_reason_out = self_reason;

    const char* strat_tok = (strat == hvac_group::ResolveStrategy::OffConflicting)
                                ? "off_conflicting" : "flip";

    // Build the exact op body once, then sign+send it to each peer.
    cJSON* opj = cJSON_CreateObject();
    cJSON_AddNumberToObject(opj, "op_id", (double)op_id);
    cJSON_AddStringToObject(opj, "target_mode", hvac_group::demand_str(target));
    cJSON_AddStringToObject(opj, "strategy", strat_tok);
    cJSON_AddStringToObject(opj, "coordinator_uid", hvac_group::self_uid().c_str());
    char* opstr = cJSON_PrintUnformatted(opj);
    cJSON_Delete(opj);
    std::string opbody = opstr ? opstr : "";
    if (opstr) cJSON_free(opstr);

    bool all_ok = sync_result_ok(self_reason);
    for (const auto& uid : g.peers) {
        std::string r = opbody.empty() ? std::string("error") : sync_peer(uid, opbody);
        if (!sync_result_ok(r)) all_ok = false;
        if (results) {
            cJSON* jm = cJSON_CreateObject();
            cJSON_AddStringToObject(jm, "uid", uid.c_str());
            cJSON_AddStringToObject(jm, "result", r.c_str());
            cJSON_AddItemToArray(results, jm);
        }
    }
    return all_ok;
}

// ── POST /api/group/resolve ─ coordinate a group mode resolution ───────
// Body: {"target_mode":"HEAT|COOL","strategy":"flip|off_conflicting"}
// Climate-only users may use off_conflicting (switch this head, turn the
// opposing zones off); flipping other zones' modes is admin-only.
// This head becomes the coordinator for one op: it allocates a monotonic op_id,
// applies the change to itself, then fans the signed op out to every enrolled
// peer. Reports "ok" only if every enrolled member converged; "partial" if any
// peer was unreachable or rejected the change.
esp_err_t handle_group_resolve(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_API_AUTH(req);
    if (slice_ctl::enabled())
        return send_json_error(req, "409 Conflict", "Tandem is enabled");
    char* body = recv_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty or oversized body");
        return ESP_FAIL;
    }
    cJSON* json = cJSON_Parse(body);
    free(body);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    const cJSON* jtm = cJSON_GetObjectItem(json, "target_mode");
    const cJSON* jst = cJSON_GetObjectItem(json, "strategy");
    std::string tm  = (jtm && cJSON_IsString(jtm)) ? jtm->valuestring : "";
    std::string stg = (jst && cJSON_IsString(jst)) ? jst->valuestring : "";
    cJSON_Delete(json);

    hvac_group::Demand target = hvac_group::parse_target_mode(tm);
    if (target != hvac_group::Demand::Heat && target != hvac_group::Demand::Cool)
        return send_json_error(req, "400 Bad Request", "target_mode must be HEAT or COOL");
    hvac_group::ResolveStrategy strat = hvac_group::parse_strategy(stg);
    if (strat != hvac_group::ResolveStrategy::OffConflicting && !admin_authorized(req))
        return send_json_error(req, "403 Forbidden", "Only an admin can switch other zones' modes");
    const char* strat_tok = (strat == hvac_group::ResolveStrategy::OffConflicting)
                                ? "off_conflicting" : "flip";

    // Coordinate self + all peers through the shared helper.
    std::string self_reason;
    cJSON* results = cJSON_CreateArray();
    bool all_ok = coordinate_group_to(target, strat, results, &self_reason);
    uint64_t op_id = hvac_group::last_op_id();

    {
        std::string verb = (strat == hvac_group::ResolveStrategy::OffConflicting)
                               ? "Turned off conflicting zones"
                               : std::string("Switched all zones to ") +
                                     pretty_token(hvac_group::demand_str(target));
        if (!all_ok) verb += " (some zones unreachable)";
        std::string who = web_actor_name(req);
        events::log(events::Cat::Group, events::Actor::WebUI, verb.c_str(),
                    who.c_str(), peer_ipv4(req));
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", all_ok ? "ok" : "partial");
    cJSON_AddNumberToObject(root, "op_id", (double)op_id);
    cJSON_AddStringToObject(root, "target_mode", hvac_group::demand_str(target));
    cJSON_AddStringToObject(root, "strategy", strat_tok);
    cJSON* jself = cJSON_CreateObject();
    cJSON_AddStringToObject(jself, "uid", hvac_group::self_uid().c_str());
    cJSON_AddStringToObject(jself, "result", self_reason.c_str());
    cJSON_AddItemToObject(root, "self", jself);
    cJSON_AddItemToObject(root, "results", results);
    char* str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, str);
    cJSON_free(str);
    cJSON_Delete(root);
    return ESP_OK;
}

// ── POST /api/group/label ─ rename the group (admin) ───────────────────
esp_err_t handle_group_label(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_ADMIN(req);
    char* body = recv_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty or oversized body");
        return ESP_FAIL;
    }
    cJSON* json = cJSON_Parse(body);
    free(body);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    const cJSON* jl = cJSON_GetObjectItem(json, "label");
    std::string label = (jl && cJSON_IsString(jl)) ? jl->valuestring : "";
    cJSON_Delete(json);

    esp_err_t err = hvac_group::set_label(label);
    if (err == ESP_ERR_INVALID_STATE)
        return send_json_error(req, "409 Conflict", "not in a group");
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS write failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

// ── POST /api/group/slice ─ time-slicing settings (admin, #133) ────────
// Body: any of {"enabled":bool,"min_slice_s":n,"max_slice_s":n,"settle_s":n}.
// Values are clamped (slice::sanitize). The config replicates to every head.
// Enabling requires every member to be online and slice-capable.
esp_err_t handle_group_slice(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_ADMIN(req);
    if (!hvac_group::in_group())
        return send_json_error(req, "409 Conflict", "not in a group");
    char* body = recv_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty or oversized body");
        return ESP_FAIL;
    }
    cJSON* json = cJSON_Parse(body);
    free(body);
    if (!json || !cJSON_IsObject(json)) {
        if (json) cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    slice_ctl::Status cur = slice_ctl::status();
    slice::Config c = cur.cfg;
    const bool was = c.enabled;
    const cJSON* v;
    if ((v = cJSON_GetObjectItem(json, "enabled")) && cJSON_IsBool(v))
        c.enabled = cJSON_IsTrue(v);
    if ((v = cJSON_GetObjectItem(json, "min_slice_s")) && cJSON_IsNumber(v) && v->valuedouble >= 0)
        c.min_slice_s = (uint32_t)v->valuedouble;
    if ((v = cJSON_GetObjectItem(json, "max_slice_s")) && cJSON_IsNumber(v) && v->valuedouble >= 0)
        c.max_slice_s = (uint32_t)v->valuedouble;
    if ((v = cJSON_GetObjectItem(json, "settle_s")) && cJSON_IsNumber(v) && v->valuedouble >= 0)
        c.settle_s = (uint32_t)v->valuedouble;
    cJSON_Delete(json);
    c = slice::sanitize(c);

    if (c.enabled && !was && !cur.all_capable)
        return send_json_error(req, "409 Conflict",
                               "every member must be online and support Tandem");
    esp_err_t err = hvac_group::set_slice_config(slice::encode_config(c));
    if (err == ESP_ERR_INVALID_STATE)
        return send_json_error(req, "409 Conflict", "not in a group");
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS write failed");
        return ESP_FAIL;
    }
    if (c.enabled != was) {
        std::string who = web_actor_name(req);
        events::log(events::Cat::Group, events::Actor::WebUI,
                    c.enabled ? "Tandem enabled" : "Tandem disabled",
                    who.empty() ? "" : who.c_str(), peer_ipv4(req));
    }
    slice_ctl::tick();  // apply now rather than on the next 1 Hz tick
    cJSON* root = slice_status_json();
    char* str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, str);
    cJSON_free(str);
    cJSON_Delete(root);
    return ESP_OK;
}

// ── POST /api/group/member/remove ─ evict a member (admin) ─────────────
// Body: {"uid":"<peer>"}. Removes the peer from the replicated membership
// (OR-Set tombstone); the change gossips to every head, and the evicted head
// drops the group on its next poll. Removing self is rejected (use /leave).
esp_err_t handle_group_member_remove(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_ADMIN(req);
    char* body = recv_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty or oversized body");
        return ESP_FAIL;
    }
    cJSON* json = cJSON_Parse(body);
    free(body);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    const cJSON* ju = cJSON_GetObjectItem(json, "uid");
    std::string uid = (ju && cJSON_IsString(ju)) ? ju->valuestring : "";
    cJSON_Delete(json);
    if (!hvac_group::is_valid_uid(uid))
        return send_json_error(req, "400 Bad Request", "invalid uid");

    esp_err_t err = hvac_group::remove_member(uid);
    if (err == ESP_ERR_INVALID_STATE)
        return send_json_error(req, "409 Conflict", "not in a group");
    if (err == ESP_ERR_INVALID_ARG)
        return send_json_error(req, "400 Bad Request", "cannot remove self; use leave");
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS write failed");
        return ESP_FAIL;
    }
    {
        std::string rn = hvac_group::member_display_name(uid);
        if (rn.empty()) rn = uid;
        std::string who = web_actor_name(req);
        events::log(events::Cat::Membership, events::Actor::WebUI,
                    (std::string("Removed ") + rn + " from the group").c_str(),
                    who.c_str(), peer_ipv4(req));
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

// ── POST /api/group/member/remove handled above. There is intentionally no
// member-rename endpoint: a head is named solely by its own device display
// name (POST /api/device), which propagates to peers via the replica. A single
// name per head, set in one place, avoids a second competing name authority.

esp_err_t handle_wifi_get(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_ADMIN(req);
    wifi::Mode m = wifi::get_mode();
    const char* mode = m == wifi::Mode::CONNECTED    ? "connected"
                     : m == wifi::Mode::CONNECTING   ? "connecting"
                     : m == wifi::Mode::PROVISIONING ? "provisioning"
                                                     : "idle";
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ssid", wifi::get_ssid());
    cJSON_AddStringToObject(root, "mode", mode);
    cJSON_AddBoolToObject(root, "connected", wifi::is_connected());
    cJSON_AddNumberToObject(root, "auth", wifi::get_auth());
    cJSON_AddStringToObject(root, "ip", wifi::get_ip());
    cJSON_AddStringToObject(root, "ap_name", wifi::get_ap_name());
    cJSON_AddBoolToObject(root, "password_set", wifi::has_password());
    char* str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, str);
    cJSON_free(str);
    cJSON_Delete(root);
    return ESP_OK;
}

// ── GET /api/scan — scan nearby WiFi networks (admin only) ────────────
esp_err_t handle_scan(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_ADMIN(req);
    std::string json;
    if (wifi::scan_json(json) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json.c_str());
    return ESP_OK;
}

// ── POST /api/wifi — set network credentials to NVS, then reboot ───────
esp_err_t handle_wifi_post(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_ADMIN(req);
    char* body = recv_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty or oversized body");
        return ESP_FAIL;
    }
    cJSON* json = cJSON_Parse(body);
    free(body);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    const cJSON* jssid = cJSON_GetObjectItem(json, "ssid");
    const cJSON* jpass = cJSON_GetObjectItem(json, "password");
    std::string ssid = (jssid && cJSON_IsString(jssid)) ? jssid->valuestring : "";
    // Preserve the stored password when the field is omitted/blank (so the user
    // can change only the SSID without re-typing the key).
    std::string pass;
    if (jpass && cJSON_IsString(jpass) && jpass->valuestring[0] != '\0')
        pass = jpass->valuestring;
    else
        pass = wifi::get_password();
    cJSON_Delete(json);

    if (ssid.empty()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid is required");
        return ESP_FAIL;
    }

    esp_err_t err = wifi::save_credentials(ssid.c_str(), pass.c_str());
    httpd_resp_set_type(req, "application/json");
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS write failed");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "{\"status\":\"saved\",\"message\":\"rebooting\"}");
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;  // unreachable
}

// ── POST /api/login ────────────────────────────────────────────────────
esp_err_t handle_login(httpd_req_t* req) {
    set_cors(req);
    char* body = recv_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty or oversized body");
        return ESP_FAIL;
    }
    cJSON* json = cJSON_Parse(body);
    free(body);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    const cJSON* jp = cJSON_GetObjectItem(json, "password");
    const char* pass = (jp && cJSON_IsString(jp)) ? jp->valuestring : "";

    char token[AUTH_SESSION_TOKEN_LEN + 1] = {0};
    auth_role_t role = auth_mgr_login(pass, token);
    cJSON_Delete(json);

    httpd_resp_set_type(req, "application/json");
    if (role == AUTH_ROLE_NONE) {
        int wait = auth_mgr_login_lockout_secs();
        if (wait > 0) {
            char out[96];
            snprintf(out, sizeof(out),
                     "{\"error\":\"too many attempts\",\"retry_after\":%d}", wait);
            httpd_resp_set_status(req, "429 Too Many Requests");
            return httpd_resp_sendstr(req, out);
        }
        httpd_resp_set_status(req, "401 Unauthorized");
        return httpd_resp_sendstr(req, "{\"error\":\"invalid password\"}");
    }
    if (token[0] != '\0') set_session_cookie(req, token);
    const char* rname = role == AUTH_ROLE_ADMIN ? "admin" : "user";
    char out[48];
    snprintf(out, sizeof(out), "{\"status\":\"ok\",\"role\":\"%s\"}", rname);
    return httpd_resp_sendstr(req, out);
}

// ── POST /api/logout ───────────────────────────────────────────────────
esp_err_t handle_logout(httpd_req_t* req) {
    set_cors(req);
    char tok[AUTH_SESSION_TOKEN_LEN + 1];
    if (get_cookie_token(req, tok, sizeof(tok))) auth_mgr_logout(tok);
    clear_session_cookie(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

// ── Admin sign-in handoff between heads of one group ───────────────────
// Opening another zone from the Zones tab lands on a different origin, so the
// admin cookie doesn't follow. The admin's head mints a ticket
//   issuer.target.exp.nonce.sig   sig = HMAC(subkey "hl-sso1", fields)
// that the target redeems once for its own admin session. The group secret is
// already what heads trust to command each other, and every member shares the
// admin password. Admin only: the climate-only user password is per head.
constexpr int64_t kSsoLifeS = 60;
constexpr int64_t kSsoSkewS = 30;     // tolerated clock difference between heads
constexpr time_t  kSsoMinEpoch = 1735689600;  // 2025-01-01: NTP has synced

struct SsoSeen { std::string nonce; int64_t until = 0; };
SsoSeen s_sso_seen[16];

std::string sso_sig(const std::string& issuer, const std::string& target, int64_t exp,
                    const std::string& nonce) {
    uint8_t k[32];
    if (!hvac_group::group_subkey("hl-sso1", k)) return "";
    return hmac_key_hex(k, "hl-sso1|" + issuer + "|" + target + "|" +
                               std::to_string((long long)exp) + "|" + nonce);
}

// One-shot: false if @p nonce was already redeemed while still valid.
bool sso_nonce_take(const std::string& nonce, int64_t until, int64_t now) {
    SsoSeen* slot = &s_sso_seen[0];
    for (auto& e : s_sso_seen) {
        if (e.until > now && e.nonce == nonce) return false;
        if (e.until < slot->until) slot = &e;
    }
    slot->nonce = nonce;
    slot->until = until;
    return true;
}

// ── POST /api/group/handoff {"uid"} ─ admin: ticket for a group member ──
esp_err_t handle_group_handoff(httpd_req_t* req) {
    REQUIRE_ADMIN(req);
    set_cors(req);
    char* body = recv_body(req);
    cJSON* json = body ? cJSON_Parse(body) : nullptr;
    free(body);
    const cJSON* ju = json ? cJSON_GetObjectItem(json, "uid") : nullptr;
    const std::string target = (ju && cJSON_IsString(ju)) ? ju->valuestring : "";
    cJSON_Delete(json);
    httpd_resp_set_type(req, "application/json");
    if (!hvac_group::is_peer(target)) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req, "{\"error\":\"not a member of this outdoor unit\"}");
    }
    const time_t now = time(nullptr);
    if (now < kSsoMinEpoch) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "{\"error\":\"clock not set\"}");
    }
    const std::string issuer = hvac_group::self_uid();
    const int64_t exp = (int64_t)now + kSsoLifeS;
    const std::string nonce = hvac_group::generate_nonce();
    const std::string sig = sso_sig(issuer, target, exp, nonce);
    if (sig.empty()) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"error\":\"not in a group\"}");
    }
    const std::string ticket = issuer + "." + target + "." +
                               std::to_string((long long)exp) + "." + nonce + "." + sig;
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ticket", ticket.c_str());
    char* out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    esp_err_t r = httpd_resp_sendstr(req, out ? out : "{}");
    free(out);
    return r;
}

// ── POST /api/sso {"ticket"} ─ redeem a handoff ticket (NO auth) ────────
esp_err_t handle_sso(httpd_req_t* req) {
    set_cors(req);
    httpd_resp_set_type(req, "application/json");
    char cur[AUTH_SESSION_TOKEN_LEN + 1];
    const bool have_cookie = get_cookie_token(req, cur, sizeof(cur));
    if (!auth_mgr_web_auth_enabled() ||
        (have_cookie && auth_mgr_session_role(cur) == AUTH_ROLE_ADMIN))
        return httpd_resp_sendstr(req, "{\"status\":\"ok\",\"role\":\"admin\"}");
    if (auth_mgr_login_lockout_secs() > 0) {
        httpd_resp_set_status(req, "429 Too Many Requests");
        return httpd_resp_sendstr(req, "{\"error\":\"too many attempts\"}");
    }
    char* body = recv_body(req);
    cJSON* json = body ? cJSON_Parse(body) : nullptr;
    free(body);
    const cJSON* jt = json ? cJSON_GetObjectItem(json, "ticket") : nullptr;
    const std::string ticket = (jt && cJSON_IsString(jt)) ? jt->valuestring : "";
    cJSON_Delete(json);

    std::vector<std::string> f;
    for (size_t pos = 0;;) {
        const size_t dot = ticket.find('.', pos);
        f.push_back(ticket.substr(pos, dot == std::string::npos ? dot : dot - pos));
        if (dot == std::string::npos || f.size() > 5) break;
        pos = dot + 1;
    }
    if (f.size() != 5) f.assign(5, "");
    const std::string& issuer = f[0];
    const std::string& target = f[1];
    const std::string& nonce = f[3];
    char* end = nullptr;
    const long long exp = f[2].empty() ? 0 : strtoll(f[2].c_str(), &end, 10);
    const int64_t now = (int64_t)time(nullptr);
    const char* why = nullptr;
    if (!end || *end != '\0' || nonce.empty() || f[4].empty())
        why = "malformed";
    else if (target != hvac_group::self_uid() || !hvac_group::is_peer(issuer))
        why = "not for this head";
    else if (now < kSsoMinEpoch)
        why = "clock not set";
    else if (exp + kSsoSkewS < now || exp > now + kSsoLifeS + kSsoSkewS)
        why = "expired";
    else {
        const std::string want = sso_sig(issuer, target, exp, nonce);
        if (want.empty() || !ct_equal(want, f[4])) {
            auth_mgr_note_failed_login();
            why = "bad signature";
        } else if (!sso_nonce_take(nonce, exp + kSsoSkewS, now)) {
            why = "already used";
        }
    }
    if (why) {
        ESP_LOGW(TAG, "sign-in handoff refused from %s: %s", peer_ipv4(req).c_str(), why);
        httpd_resp_set_status(req, "401 Unauthorized");
        char out[80];
        snprintf(out, sizeof(out), "{\"error\":\"%s\"}", why);
        return httpd_resp_sendstr(req, out);
    }
    char token[AUTH_SESSION_TOKEN_LEN + 1] = {0};
    if (!auth_mgr_create_admin_session(token)) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"no session\"}");
    }
    if (have_cookie) auth_mgr_logout(cur);  // replaces a climate-only session
    ESP_LOGI(TAG, "admin signed in by handoff from %s", issuer.c_str());
    set_session_cookie(req, token);
    return httpd_resp_sendstr(req, "{\"status\":\"ok\",\"role\":\"admin\"}");
}

// ── GET /api/auth — auth config + whether this client is authenticated ──
esp_err_t handle_auth_get(httpd_req_t* req) {
    set_cors(req);
    auth_role_t role;
    tokens::Entry te;
    const TokenAuth ta = bearer_token(req, &te);
    if (ta != TokenAuth::None) {
        role = ta == TokenAuth::Bad                 ? AUTH_ROLE_NONE
               : te.scope == tokens::Scope::Admin ? AUTH_ROLE_ADMIN
                                                    : AUTH_ROLE_USER;
    } else if (!auth_mgr_web_auth_enabled()) {
        role = AUTH_ROLE_ADMIN;  // no web login → full access
    } else {
        char tok[AUTH_SESSION_TOKEN_LEN + 1];
        role = get_cookie_token(req, tok, sizeof(tok)) ? auth_mgr_session_role(tok)
                                                       : AUTH_ROLE_NONE;
    }
    bool guest = role == AUTH_ROLE_NONE && ta == TokenAuth::None && open_climate();
    if (guest) role = AUTH_ROLE_USER;
    bool authed = role != AUTH_ROLE_NONE;
    const char* rname = role == AUTH_ROLE_ADMIN ? "admin"
                        : role == AUTH_ROLE_USER ? "user" : "none";
    cJSON* root = cJSON_CreateObject();
    auth_state_t st = auth_mgr_state();
    cJSON_AddStringToObject(root, "state", st == AUTH_STATE_CLAIMED     ? "claimed"
                                           : st == AUTH_STATE_UNCLAIMED ? "unclaimed"
                                                                        : "legacy");
    cJSON_AddNumberToObject(root, "recovery_secs_left", auth_mgr_recovery_secs_left());
    cJSON_AddBoolToObject(root, "web_auth_enabled", auth_mgr_web_auth_enabled());
    cJSON_AddBoolToObject(root, "api_auth_enabled", auth_mgr_api_auth_enabled());
    cJSON_AddStringToObject(root, "username", auth_mgr_get_username());
    cJSON_AddStringToObject(root, "name", wifi::device_display_name().c_str());
    cJSON_AddStringToObject(root, "hostname", wifi::mdns_hostname().c_str());
    cJSON_AddStringToObject(root, "role", rname);
    if (ta == TokenAuth::Ok) cJSON_AddStringToObject(root, "token_scope", tokens::scope_str(te.scope));
    cJSON_AddBoolToObject(root, "web_password_set", auth_mgr_web_password_set());
    cJSON_AddBoolToObject(root, "user_password_set", auth_mgr_user_password_set());
    cJSON_AddBoolToObject(root, "authenticated", authed);
    cJSON_AddBoolToObject(root, "open_climate", open_climate());
    cJSON_AddBoolToObject(root, "guest", guest);
    access_policy::Limits lim = access_policy::limits();
    cJSON* jl = cJSON_AddObjectToObject(root, "user_limits");
    cJSON_AddBoolToObject(jl, "enabled", lim.enabled);
    cJSON_AddNumberToObject(jl, "min", lim.min_c);
    cJSON_AddNumberToObject(jl, "max", lim.max_c);
    cJSON_AddBoolToObject(root, "power_reset", access_policy::power_reset_enabled());
    // Only reveal the API key to an admin client.
    if (role == AUTH_ROLE_ADMIN) {
        char key[AUTH_API_KEY_LEN + 1] = {0};
        if (auth_mgr_get_api_key(key)) cJSON_AddStringToObject(root, "api_key", key);
    }
    char* str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, str);
    cJSON_free(str);
    cJSON_Delete(root);
    return ESP_OK;
}

// ── POST /api/auth — update auth settings (admin only) ─────────────────
// Also the claim endpoint: {"claim":true,"password":"…","token":"…"} creates
// the first admin password on an unclaimed head (token from the setup portal),
// or replaces any admin password while the recovery window is open, and signs
// the caller in.
// On a legacy head (no password, UI open) setting "password" is a claim too.
esp_err_t handle_auth_post(httpd_req_t* req) {
    set_cors(req);
    bool authed = admin_authorized(req);  // evaluated before any claim
    char* body = recv_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty or oversized body");
        return ESP_FAIL;
    }
    cJSON* json = cJSON_Parse(body);
    free(body);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    const cJSON* v;
    const cJSON* jpw = cJSON_GetObjectItem(json, "password");
    const char* new_pw = (jpw && cJSON_IsString(jpw)) ? jpw->valuestring : "";
    bool claim = (v = cJSON_GetObjectItem(json, "claim")) && cJSON_IsTrue(v);
    if (!claim && new_pw[0] != '\0' && auth_mgr_state() == AUTH_STATE_LEGACY) claim = true;
    bool claimed = false;
    if (claim) {
        const cJSON* jt = cJSON_GetObjectItem(json, "token");
        const char* tok = (jt && cJSON_IsString(jt)) ? jt->valuestring : "";
        char session[AUTH_SESSION_TOKEN_LEN + 1] = {0};
        const bool replacing = auth_mgr_web_password_set();
        auth_claim_result_t r = auth_mgr_claim(new_pw, tok, session);
        if (r != AUTH_CLAIM_OK) {
            cJSON_Delete(json);
            httpd_resp_set_type(req, "application/json");
            switch (r) {
                case AUTH_CLAIM_ALREADY:
                    httpd_resp_set_status(req, "409 Conflict");
                    return httpd_resp_sendstr(req, "{\"error\":\"admin password already set\"}");
                case AUTH_CLAIM_DENIED:
                    httpd_resp_set_status(req, "403 Forbidden");
                    return httpd_resp_sendstr(req, "{\"error\":\"claim not allowed\"}");
                case AUTH_CLAIM_INVALID:
                    httpd_resp_set_status(req, "400 Bad Request");
                    return httpd_resp_sendstr(req,
                        "{\"error\":\"Admin password must be set and differ from the user password\"}");
                default:
                    httpd_resp_set_status(req, "500 Internal Server Error");
                    return httpd_resp_sendstr(req, "{\"error\":\"could not save password\"}");
            }
        }
        set_session_cookie(req, session);
        events::log(events::Cat::System, events::Actor::WebUI,
                    replacing ? "Admin password changed after reset" : "Admin password created",
                    "", peer_ipv4(req));
        authed = claimed = true;
    }
    if (!authed) {
        cJSON_Delete(json);
        send_unauthorized(req);
        return ESP_FAIL;
    }

    // Change the admin password (already done above when this was a claim).
    if (!claimed && new_pw[0] != '\0') {
        if (!auth_mgr_set_admin_password(new_pw)) {
            cJSON_Delete(json);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_sendstr(req,
                "{\"error\":\"Admin password must differ from the user password\"}");
        }
        // The change logged every session out; keep the caller signed in.
        char session[AUTH_SESSION_TOKEN_LEN + 1] = {0};
        if (auth_mgr_login(new_pw, session) == AUTH_ROLE_ADMIN && session[0])
            set_session_cookie(req, session);
    }

    // Climate-only "user" account: set its password, or remove it.
    if ((v = cJSON_GetObjectItem(json, "user_password")) && cJSON_IsString(v) &&
        v->valuestring[0] != '\0') {
        if (!auth_mgr_set_user_password(v->valuestring)) {
            cJSON_Delete(json);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_sendstr(req,
                "{\"error\":\"User password must differ from the admin password\"}");
        }
    }
    if ((v = cJSON_GetObjectItem(json, "remove_user")) && cJSON_IsTrue(v))
        auth_mgr_set_user_password(nullptr);

    if ((v = cJSON_GetObjectItem(json, "user_limits")) && cJSON_IsObject(v)) {
        access_policy::Limits l = access_policy::limits();
        const cJSON* f;
        if ((f = cJSON_GetObjectItem(v, "enabled")) && cJSON_IsBool(f)) l.enabled = cJSON_IsTrue(f);
        if ((f = cJSON_GetObjectItem(v, "min")) && cJSON_IsNumber(f)) l.min_c = (float)f->valuedouble;
        if ((f = cJSON_GetObjectItem(v, "max")) && cJSON_IsNumber(f)) l.max_c = (float)f->valuedouble;
        if (!access_policy::set_limits(l)) {
            cJSON_Delete(json);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_sendstr(req,
                "{\"error\":\"Limits must be 16-31 C in 0.5 steps, min not above max\"}");
        }
    }
    if ((v = cJSON_GetObjectItem(json, "power_reset")) && cJSON_IsBool(v)) {
        bool on = cJSON_IsTrue(v);
        if (on != access_policy::power_reset_enabled()) {
            access_policy::set_power_reset_enabled(on);
            events::log(events::Cat::System, events::Actor::WebUI,
                        on ? "Power-cut admin password reset turned on"
                           : "Power-cut admin password reset turned off",
                        web_actor_name(req).c_str(), peer_ipv4(req));
        }
    }

    // "web_auth_enabled" is no longer settable: login is always required once
    // an admin password exists.
    if ((v = cJSON_GetObjectItem(json, "api_auth_enabled")) && cJSON_IsBool(v))
        auth_mgr_set_api_auth_enabled(cJSON_IsTrue(v));

    char key[AUTH_API_KEY_LEN + 1] = {0};
    if ((v = cJSON_GetObjectItem(json, "regenerate_key")) && cJSON_IsTrue(v))
        auth_mgr_regenerate_api_key(key);
    else
        auth_mgr_get_api_key(key);
    cJSON_Delete(json);

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    if (claimed) cJSON_AddStringToObject(root, "role", "admin");
    cJSON_AddBoolToObject(root, "web_auth_enabled", auth_mgr_web_auth_enabled());
    cJSON_AddBoolToObject(root, "api_auth_enabled", auth_mgr_api_auth_enabled());
    cJSON_AddBoolToObject(root, "web_password_set", auth_mgr_web_password_set());
    cJSON_AddBoolToObject(root, "user_password_set", auth_mgr_user_password_set());
    if (key[0] != '\0') cJSON_AddStringToObject(root, "api_key", key);
    char* str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, str);
    cJSON_free(str);
    cJSON_Delete(root);
    return ESP_OK;
}

// ── GET /api/events (admin) ────────────────────────────────────────────
// The activity log is admin-only. Optional query: since=<seq> (only newer),
// limit=<n>. Newest-first. Fast: served from the in-RAM ring.
esp_err_t handle_events_get(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_ADMIN(req);
    uint32_t since = 0, limit = 100;
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen > 0 && qlen < 128) {
        char q[128];
        if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
            char val[16];
            if (httpd_query_key_value(q, "since", val, sizeof(val)) == ESP_OK)
                since = (uint32_t)strtoul(val, nullptr, 10);
            if (httpd_query_key_value(q, "limit", val, sizeof(val)) == ESP_OK)
                limit = (uint32_t)strtoul(val, nullptr, 10);
        }
    }
    std::string body = events::get_json(since, limit);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body.c_str());
    return ESP_OK;
}

// ── GET /api/logs (admin) ──────────────────────────────────────────────
// Recent ESP_LOG output as plain text, oldest first, for diagnosing sporadic
// problems. Survives software/panic/brownout restarts; no UI.
esp_err_t handle_logs_get(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_ADMIN(req);
    std::string body = devlog::snapshot();
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, body.data(), body.size());
    return ESP_OK;
}

// ── DELETE /api/logs (admin) ───────────────────────────────────────────
esp_err_t handle_logs_clear(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_ADMIN(req);
    devlog::clear();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// ── DELETE /api/events (admin) ─────────────────────────────────────────
esp_err_t handle_events_clear(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_ADMIN(req);
    events::clear();
    events::log(events::Cat::System, events::Actor::WebUI, "Activity log cleared",
                web_actor_name(req), peer_ipv4(req));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// ── GET /api/history (admin) ───────────────────────────────────────────
// Samples for the History chart. Query: window=<s> (default 24 h, 1 h..14 d),
// end=<unix s> (default now). Long windows are averaged into buckets so at
// most history::kMaxPoints rows come back. Streamed in chunks.
esp_err_t handle_history_get(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_API_AUTH(req);
    uint32_t end_q = 0, window = 0;
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen > 0 && qlen < 128) {
        char q[128];
        if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
            char val[16];
            if (httpd_query_key_value(q, "end", val, sizeof(val)) == ESP_OK)
                end_q = (uint32_t)strtoul(val, nullptr, 10);
            if (httpd_query_key_value(q, "window", val, sizeof(val)) == ESP_OK)
                window = (uint32_t)strtoul(val, nullptr, 10);
        }
    }
    window = history::clamp_window(window);
    const uint32_t bucket = history::bucket_for(window);
    time_t now = time(nullptr);
    const bool clock_valid = now > 1700000000;
    const uint32_t latest_end = clock_valid
        ? (uint32_t)now - (uint32_t)now % history::kIntervalS + history::kIntervalS : 0;
    const uint32_t end = (end_q && end_q < latest_end) ? end_q : latest_end;
    const uint32_t start = end > window ? end - window : 0;

    httpd_resp_set_type(req, "application/json");
    std::string buf;
    buf.reserve(3072);
    bool ok = true;
    auto send = [&](bool force) {
        if (!ok || buf.empty() || (!force && buf.size() < 2048)) return;
        ok = httpd_resp_send_chunk(req, buf.data(), buf.size()) == ESP_OK;
        buf.clear();
    };
    char row[96];
    snprintf(row, sizeof(row),
             "{\"clock_valid\":%s,\"now\":%lu,\"start\":%lu,\"end\":%lu,\"window\":%lu,",
             clock_valid ? "true" : "false", (unsigned long)(clock_valid ? now : 0),
             (unsigned long)start, (unsigned long)end, (unsigned long)window);
    buf += row;
    snprintf(row, sizeof(row),
             "\"interval\":%lu,\"bucket\":%lu,\"earliest\":%lu,\"latest_end\":%lu,",
             (unsigned long)history::kIntervalS, (unsigned long)bucket,
             (unsigned long)history_ctl::earliest(), (unsigned long)latest_end);
    buf += row;
    buf += "\"fields\":";
    buf += history::kFieldsJson;
    buf += ",\"samples\":[";

    bool first = true;
    auto emit = [&](const history::Record& r) {
        history::format_row(r, row, sizeof(row));
        if (!first) buf += ',';
        first = false;
        buf += row;
        send(false);
    };
    if (clock_valid) {
        history::Bucketer b(bucket);
        history::Record out;
        history_ctl::query(start, end, [&](const history::Record& r) {
            if (b.add(r, out)) emit(out);
        });
        if (b.finish(out)) emit(out);
    }
    buf += "]}";
    send(true);
    if (ok) httpd_resp_send_chunk(req, nullptr, 0);
    return ok ? ESP_OK : ESP_FAIL;
}

// ── OPTIONS /api/* (CORS preflight) ────────────────────────────────────
// ── GET /api/info ─ public identity + capabilities, for discovery ───────
// Unauthenticated: lets an integration recognise a HeatLink head, check the
// API version and learn which sign-in methods apply before it has a token.
esp_err_t handle_info(httpd_req_t* req) {
    set_cors(req);
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "uid", wifi::device_uid().c_str());
    cJSON_AddStringToObject(o, "name", wifi::device_display_name().c_str());
    cJSON_AddStringToObject(o, "hostname", wifi::mdns_hostname().c_str());
    cJSON_AddStringToObject(o, "model", "stamp-s3");
    cJSON_AddStringToObject(o, "fw", esp_app_get_description()->version);
    cJSON_AddNumberToObject(o, "api_version", kApiVersion);
    cJSON_AddNumberToObject(o, "pv", hvac_group::kProtocolVersion);
    auth_state_t st = auth_mgr_state();
    cJSON* ja = cJSON_AddObjectToObject(o, "auth");
    cJSON_AddStringToObject(ja, "state", st == AUTH_STATE_CLAIMED     ? "claimed"
                                         : st == AUTH_STATE_UNCLAIMED ? "unclaimed"
                                                                      : "legacy");
    cJSON_AddBoolToObject(ja, "web_login", auth_mgr_web_auth_enabled());
    cJSON_AddBoolToObject(ja, "open_climate", open_climate());
    cJSON_AddBoolToObject(ja, "api_key", auth_mgr_api_auth_enabled());
    cJSON_AddBoolToObject(ja, "tokens", true);
    cJSON* jf = cJSON_AddArrayToObject(o, "features");
    for (const char* f : {"tokens", "events", "history", "park", "room_sensors", "group",
                          "tandem"})
        cJSON_AddItemToArray(jf, cJSON_CreateString(f));
    std::string gh = hvac_group::group_fingerprint();
    if (!gh.empty()) {
        cJSON* jg = cJSON_AddObjectToObject(o, "group");
        cJSON_AddStringToObject(jg, "ghash", gh.c_str());
        cJSON_AddStringToObject(jg, "label", hvac_group::get().group_label.c_str());
    }
    char* str = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, str ? str : "{}");
    cJSON_free(str);
    return ESP_OK;
}

// ── Integration tokens (admin) ──────────────────────────────────────────
cJSON* token_json(const tokens::Entry& e) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", e.id.c_str());
    cJSON_AddStringToObject(o, "label", e.label.c_str());
    cJSON_AddStringToObject(o, "scope", tokens::scope_str(e.scope));
    if (e.created) cJSON_AddNumberToObject(o, "created", e.created);
    else cJSON_AddNullToObject(o, "created");
    if (e.last_used) cJSON_AddNumberToObject(o, "last_used", e.last_used);
    else cJSON_AddNullToObject(o, "last_used");
    return o;
}

esp_err_t send_cjson(httpd_req_t* req, cJSON* o) {
    char* str = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_sendstr(req, str ? str : "{}");
    cJSON_free(str);
    return r;
}

// GET /api/tokens
esp_err_t handle_tokens_get(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_ADMIN(req);
    cJSON* o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "max", tokens::kMaxTokens);
    cJSON* arr = cJSON_AddArrayToObject(o, "tokens");
    for (const auto& e : token_store_list()) cJSON_AddItemToArray(arr, token_json(e));
    return send_cjson(req, o);
}

// POST /api/tokens {"label","scope"} → the token text, shown only this once.
esp_err_t handle_tokens_post(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_ADMIN(req);
    char* body = recv_body(req);
    cJSON* json = body ? cJSON_Parse(body) : nullptr;
    free(body);
    const cJSON* jl = json ? cJSON_GetObjectItem(json, "label") : nullptr;
    const cJSON* js = json ? cJSON_GetObjectItem(json, "scope") : nullptr;
    tokens::Scope scope = tokens::Scope::Control;
    std::string label = cJSON_IsString(jl) ? jl->valuestring : "";
    bool scope_ok = js == nullptr || (cJSON_IsString(js) && tokens::parse_scope(js->valuestring, &scope));
    cJSON_Delete(json);
    if (tokens::sanitize_label(label).empty() || !scope_ok)
        return send_json_error(req, "400 Bad Request",
                               "label is required; scope must be read, control or admin");
    if (token_store_list().size() >= tokens::kMaxTokens)
        return send_json_error(req, "409 Conflict", "token limit reached; revoke one first");
    std::string text;
    tokens::Entry e;
    if (!token_store_create(label, scope, &text, &e))
        return send_json_error(req, "500 Internal Server Error", "could not create token");
    events::log(events::Cat::System, events::Actor::WebUI,
                ("Integration token \"" + e.label + "\" created (" + tokens::scope_str(scope) +
                 ")").c_str(),
                web_actor_name(req).c_str(), peer_ipv4(req));
    cJSON* o = token_json(e);
    cJSON_AddStringToObject(o, "token", text.c_str());
    httpd_resp_set_status(req, "201 Created");
    return send_cjson(req, o);
}

// DELETE /api/tokens?id=<id>
esp_err_t handle_tokens_delete(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_ADMIN(req);
    char q[64] = "", id[tokens::kIdLen + 2] = "";
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK)
        httpd_query_key_value(q, "id", id, sizeof(id));
    std::string label;
    for (const auto& e : token_store_list())
        if (e.id == id) label = e.label;
    // Name the actor before revoking: a token may be revoking itself.
    std::string who = web_actor_name(req);
    if (label.empty() || !token_store_revoke(id))
        return send_json_error(req, "404 Not Found", "no such token");
    events::log(events::Cat::System, events::Actor::WebUI,
                ("Integration token \"" + label + "\" revoked").c_str(), who.c_str(),
                peer_ipv4(req));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

esp_err_t handle_options(httpd_req_t* req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET,POST,DELETE,OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, Authorization");
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, nullptr, 0);
}

}  // namespace

// Group-aware handling of a climate mode command that arrived over MQTT (Home
// Assistant). HA offers no confirm dialog, so a Heat/Cool command on a grouped
// head that would conflict with a peer is promoted to a whole-group switch —
// the same coordinated resolve the web UI runs after its "switch all zones"
// confirm. Mirrors the web UI's needsGroupSwitch(): promote only when a KNOWN
// peer holds the opposite mode (or the unit is inferred locked the other way);
// otherwise the caller applies the command locally as usual. Returns true if it
// was handled as a group switch.
bool try_group_mode_command(const std::string& mode) {
    hvac_group::Demand target;
    if (strcasecmp(mode.c_str(), "heat") == 0)      target = hvac_group::Demand::Heat;
    else if (strcasecmp(mode.c_str(), "cool") == 0) target = hvac_group::Demand::Cool;
    else return false;                       // only HEAT/COOL can conflict
    if (!hvac_group::in_group()) return false;
    if (slice_ctl::enabled()) return false;  // mixed modes are time-sliced, not switched

    std::vector<hvac_group::MemberObs> members;
    hvac_group::GroupView view = compute_group_view(&members);
    const hvac_group::Demand opp = (target == hvac_group::Demand::Heat)
                                       ? hvac_group::Demand::Cool
                                       : hvac_group::Demand::Heat;
    bool conflict = (view.locked_mode == opp);
    for (size_t i = 1; !conflict && i < members.size(); ++i)
        conflict = (members[i].state == hvac_group::MemberState::Known &&
                    members[i].demand == opp);
    if (!conflict) return false;             // no conflict → apply locally

    ESP_LOGI(TAG, "HA mode '%s' conflicts on grouped head -> switching all zones to %s",
             mode.c_str(), hvac_group::demand_str(target));
    coordinate_group_to(target, hvac_group::ResolveStrategy::FlipMode, nullptr, nullptr);
    events::log(events::Cat::Group, events::Actor::HomeAssistant,
                (std::string("Switched all zones to ") +
                 pretty_token(hvac_group::demand_str(target))).c_str());
    return true;
}

namespace {

// ── House view (issue #131) ────────────────────────────────────────────
// Lists every head on the LAN across all outdoor units. Cross-group data comes
// from an unauthenticated, read-only summary: it never touches the group secret
// and exposes nothing usable for control. The aggregator runs in its own task
// so the (multi-second) mDNS browse + peer fetches never block the httpd thread.

void refresh_house_advert() {
    std::string glabel;
    if (hvac_group::in_group()) glabel = hvac_group::get().group_label;
    wifi::set_house_advert(wifi::device_display_name(),
                           hvac_group::group_fingerprint(), glabel);
}

// Read-only snapshot of this head for the House view.
cJSON* build_house_summary() {
    cn105::Settings st  = s_hooks.get_settings ? s_hooks.get_settings() : cn105::Settings{};
    cn105::Status   sta = s_hooks.get_status   ? s_hooks.get_status()   : cn105::Status{};
    const bool connected = s_hooks.unit_connected && s_hooks.unit_connected();
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "uid", wifi::device_uid().c_str());
    cJSON_AddStringToObject(o, "name", wifi::device_display_name().c_str());
    cJSON_AddNumberToObject(o, "pv", hvac_group::kProtocolVersion);
    cJSON_AddStringToObject(o, "version", esp_app_get_description()->version);
    std::string gh = hvac_group::group_fingerprint();
    if (!gh.empty()) {
        cJSON_AddStringToObject(o, "ghash", gh.c_str());
        cJSON_AddStringToObject(o, "glabel", hvac_group::get().group_label.c_str());
    }
    cJSON_AddBoolToObject(o, "unit_connected", connected);
    if (connected) {
        cJSON_AddStringToObject(o, "power", st.power.c_str());
        cJSON_AddStringToObject(o, "mode", st.mode.c_str());
        cJSON_AddBoolToObject(o, "operating", sta.operating);
        const char* rs = "";
        cJSON_AddNumberToObject(o, "roomTemp", display_room_temp(sta, &rs));
        if (*rs) cJSON_AddStringToObject(o, "rs", rs);
        cJSON_AddNumberToObject(o, "targetTemp", st.temperature);
        cJSON_AddNumberToObject(o, "freq", sta.compressorFrequency);
    }
    return o;
}

// ── GET /api/house/summary — public, read-only ─────────────────────────
esp_err_t handle_house_summary(httpd_req_t* req) {
    set_cors(req);
    cJSON* o = build_house_summary();
    char* str = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, str ? str : "{}");
    cJSON_free(str);
    return ESP_OK;
}

constexpr int64_t kHouseCacheUs  = 15LL * 1000000;  // rescan when older
constexpr int     kHouseFetchMs  = 1500;
constexpr size_t  kHouseBodyMax  = 1024;

struct HouseCache {
    std::string heads_json = "[]";  // last completed scan (JSON array)
    int64_t     at_us      = 0;     // 0 = never scanned
    bool        scanning   = false;
};
HouseCache        s_house;
SemaphoreHandle_t s_house_mtx  = nullptr;
TaskHandle_t      s_house_task = nullptr;

esp_err_t house_evt(esp_http_client_event_t* evt) {
    auto* body = static_cast<std::string*>(evt->user_data);
    if (body && evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->data_len > 0 &&
        body->size() < kHouseBodyMax)
        body->append(static_cast<const char*>(evt->data), evt->data_len);
    return ESP_OK;
}

// Fetch one head's /api/house/summary. Returns the HTTP status (0 = unreachable).
int fetch_house_summary(const std::string& ip, std::string& body) {
    std::string url = "http://" + ip + "/api/house/summary";
    esp_http_client_config_t cfg = {};
    cfg.url           = url.c_str();
    cfg.method        = HTTP_METHOD_GET;
    cfg.timeout_ms    = kHouseFetchMs;
    cfg.event_handler = house_evt;
    cfg.user_data     = &body;
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return 0;
    int status = 0;
    if (esp_http_client_perform(cli) == ESP_OK) status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);
    return status;
}

void copy_if(cJSON* dst, const cJSON* src, const char* key) {
    const cJSON* v = cJSON_GetObjectItem(src, key);
    if (!v) return;
    if (cJSON_GetObjectItem(dst, key)) cJSON_DeleteItemFromObject(dst, key);
    cJSON_AddItemToObject(dst, key, cJSON_Duplicate(v, true));
}

void house_task(void*) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        std::vector<wifi::HeadAdvert> heads;
        wifi::browse_heads(heads);
        cJSON* arr = cJSON_CreateArray();
        const std::string self_gh = hvac_group::group_fingerprint();
        std::string self_gl;
        if (!self_gh.empty()) self_gl = hvac_group::get().group_label;
        for (auto h : heads) {
            // An enrolled peer is on our outdoor unit by definition; fill gaps
            // from our own group records (covers peers on older firmware that
            // don't advertise name/ghash yet).
            if (!self_gh.empty() && hvac_group::is_peer(h.uid)) {
                h.ghash = self_gh;
                h.glabel = self_gl;
                if (h.name.empty()) h.name = hvac_group::member_display_name(h.uid);
            }
            cJSON* o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "uid", h.uid.c_str());
            cJSON_AddStringToObject(o, "name", (h.name.empty() ? h.uid : h.name).c_str());
            cJSON_AddStringToObject(o, "ip", h.ip.c_str());
            if (!h.ghash.empty())  cJSON_AddStringToObject(o, "ghash", h.ghash.c_str());
            if (!h.glabel.empty()) cJSON_AddStringToObject(o, "glabel", h.glabel.c_str());
            if (h.pv >= 0) cJSON_AddNumberToObject(o, "pv", h.pv);
            std::string body;
            int status = fetch_house_summary(h.ip, body);
            cJSON* sj = (status == 200) ? cJSON_Parse(body.c_str()) : nullptr;
            const bool ok = sj && cJSON_IsObject(sj);
            cJSON_AddBoolToObject(o, "reachable", status != 0);
            cJSON_AddBoolToObject(o, "summary_supported", ok);
            if (ok) {
                // The summary is fresher than the mDNS TXT; it wins where present.
                static const char* keys[] = {"name", "ghash", "glabel", "pv", "version",
                                             "unit_connected", "power", "mode",
                                             "operating", "roomTemp", "targetTemp", "rs",
                                             "freq"};
                for (const char* k : keys) copy_if(o, sj, k);
            }
            cJSON_Delete(sj);
            cJSON_AddItemToArray(arr, o);
        }
        char* str = cJSON_PrintUnformatted(arr);
        cJSON_Delete(arr);
        xSemaphoreTake(s_house_mtx, portMAX_DELAY);
        s_house.heads_json = str ? str : "[]";
        s_house.at_us      = esp_timer_get_time();
        s_house.scanning   = false;
        xSemaphoreGive(s_house_mtx);
        cJSON_free(str);
    }
}

// ── GET /api/house — every head on the LAN, across outdoor units ────────
// Returns the last completed scan immediately and kicks a background rescan
// when it is stale; "scanning" tells the UI to poll again shortly.
esp_err_t send_house(httpd_req_t* req) {
    std::string heads;
    int64_t at = 0;
    bool scanning = false;
    if (s_house_mtx) {
        xSemaphoreTake(s_house_mtx, portMAX_DELAY);
        const int64_t now = esp_timer_get_time();
        if (!s_house.scanning && s_house_task &&
            (s_house.at_us == 0 || now - s_house.at_us > kHouseCacheUs)) {
            s_house.scanning = true;
            xTaskNotifyGive(s_house_task);
        }
        heads = s_house.heads_json;
        at = s_house.at_us;
        scanning = s_house.scanning;
        xSemaphoreGive(s_house_mtx);
    }
    cJSON* root = cJSON_CreateObject();
    cJSON* self = build_house_summary();
    cJSON_AddStringToObject(self, "ip", wifi::get_ip());
    cJSON_AddItemToObject(root, "self", self);
    cJSON* ha = cJSON_Parse(heads.c_str());
    cJSON_AddItemToObject(root, "heads", ha ? ha : cJSON_CreateArray());
    cJSON_AddBoolToObject(root, "scanning", scanning);
    if (at) cJSON_AddNumberToObject(root, "age_s",
                                    (double)((esp_timer_get_time() - at) / 1000000));
    char* str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, str ? str : "{}");
    cJSON_free(str);
    return ESP_OK;
}

esp_err_t handle_house_get(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_API_AUTH(req);
    return send_house(req);
}

// ── GET /api/group/discover?t= ─ heads to offer for joining ─────────────
// Same scan as /api/house, but also allowed before sign-in during setup (claim
// token in @c t) or the reset window, so a new head can list groups to join.
esp_err_t handle_group_discover(httpd_req_t* req) {
    set_cors(req);
    if (!admin_authorized(req)) {
        char q[128] = "", t[AUTH_CLAIM_TOKEN_LEN + 2] = "";
        if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK)
            httpd_query_key_value(q, "t", t, sizeof(t));
        if (!auth_mgr_join_authorized(t)) {
            send_unauthorized(req);
            return ESP_FAIL;
        }
    }
    return send_house(req);
}

}  // namespace

esp_err_t init(const Hooks& hooks) {
    s_hooks = hooks;
    auth_mgr_init();
    token_store_init();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 70;  // must exceed the uris[] count below
    cfg.stack_size = 8192;
    // The default 7 sockets reserve 3 for internal use, leaving only 4 for
    // clients — fewer than the 6 keep-alive connections a single browser tab
    // opens, which wedged the server (ERR_EMPTY_RESPONSE). Give real headroom.
    cfg.max_open_sockets = 13;
    // Recycle the least-recently-used connection instead of refusing new ones.
    cfg.lru_purge_enable = true;
    // Reap dead/abandoned connections (closed tab, slept laptop) via TCP
    // keepalive so their sockets don't linger and starve the pool.
    cfg.keep_alive_enable   = true;
    cfg.keep_alive_idle     = 5;   // seconds idle before first probe
    cfg.keep_alive_interval = 5;   // seconds between probes
    cfg.keep_alive_count    = 3;   // drop after 3 unanswered probes

    esp_err_t ret = httpd_start(&s_server, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    const httpd_uri_t uris[] = {
        {"/",                        HTTP_GET,     handle_root,           nullptr},
        {"/manifest.webmanifest",    HTTP_GET,     handle_manifest,       nullptr},
        {"/icon-180.png",            HTTP_GET,     handle_icon_180,       nullptr},
        {"/icon-192.png",            HTTP_GET,     handle_icon_192,       nullptr},
        {"/icon-512.png",            HTTP_GET,     handle_icon_512,       nullptr},
        {"/api/status",              HTTP_GET,     handle_status,         nullptr},
        {"/api/settings",            HTTP_GET,     handle_get_settings,   nullptr},
        {"/api/settings",            HTTP_POST,    handle_post_settings,  nullptr},
        {"/api/capabilities",        HTTP_GET,     handle_capabilities_get,  nullptr},
        {"/api/capabilities",        HTTP_POST,    handle_capabilities_post, nullptr},
        {"/api/park",                HTTP_GET,     handle_park_get,       nullptr},
        {"/api/park",                HTTP_POST,    handle_park_post,      nullptr},
        {"/api/sensors",             HTTP_GET,     handle_sensors_get,    nullptr},
        {"/api/sensors",             HTTP_POST,    handle_sensors_post,   nullptr},
        {"/api/sensors/report",      HTTP_GET,     handle_sensors_report, nullptr},
        {"/api/ota",                 HTTP_POST,    handle_ota_upload,     nullptr},
        {"/api/ota/url",             HTTP_POST,    handle_ota_url,        nullptr},
        {"/api/ota/status",          HTTP_GET,     handle_ota_status,     nullptr},
        {"/api/update",              HTTP_GET,     handle_update_get,     nullptr},
        {"/api/update/check",        HTTP_POST,    handle_update_check,   nullptr},
        {"/api/update/install",      HTTP_POST,    handle_update_install, nullptr},
        {"/api/diag/reset-brownout", HTTP_POST,    handle_diag_reset_brownout, nullptr},
        {"/api/diag/reset-wifi-drops", HTTP_POST,  handle_diag_reset_wifi_drops, nullptr},
        {"/api/diag/reset-vin-min",  HTTP_POST,    handle_diag_reset_vin_min, nullptr},
        {"/api/mqtt",                HTTP_GET,      handle_mqtt_get,       nullptr},
        {"/api/mqtt",                HTTP_POST,     handle_mqtt_post,      nullptr},
        {"/api/device",              HTTP_POST,     handle_device_post,    nullptr},
        {"/api/group",               HTTP_GET,      handle_group_get,      nullptr},
        {"/api/group/state",         HTTP_GET,      handle_group_state,    nullptr},
        {"/api/group/join/params",   HTTP_GET,      handle_group_join_params, nullptr},
        {"/api/group/join/claim",    HTTP_POST,     handle_group_join_claim,  nullptr},
        {"/api/group/join",          HTTP_POST,     handle_group_join,        nullptr},
        {"/api/group/discover",      HTTP_GET,      handle_group_discover,    nullptr},
        {"/api/group/leave",         HTTP_POST,     handle_group_leave,       nullptr},
        {"/api/group/sync",          HTTP_POST,     handle_group_sync,        nullptr},
        {"/api/group/resolve",       HTTP_POST,     handle_group_resolve,     nullptr},
        {"/api/group/label",         HTTP_POST,     handle_group_label,       nullptr},
        {"/api/group/slice",         HTTP_POST,     handle_group_slice,       nullptr},
        {"/api/group/member/remove", HTTP_POST,     handle_group_member_remove, nullptr},
        {"/api/house",               HTTP_GET,      handle_house_get,      nullptr},
        {"/api/house/summary",       HTTP_GET,      handle_house_summary,  nullptr},
        {"/api/wifi",                HTTP_GET,      handle_wifi_get,       nullptr},
        {"/api/scan",                HTTP_GET,      handle_scan,           nullptr},
        {"/api/wifi",                HTTP_POST,     handle_wifi_post,      nullptr},
        {"/api/system/restart",      HTTP_POST,    handle_restart,        nullptr},
        {"/api/system/factory_reset",HTTP_POST,    handle_factory_reset,  nullptr},
        {"/api/login",               HTTP_POST,    handle_login,          nullptr},
        {"/api/logout",              HTTP_POST,    handle_logout,         nullptr},
        {"/api/sso",                 HTTP_POST,    handle_sso,            nullptr},
        {"/api/group/handoff",       HTTP_POST,    handle_group_handoff,  nullptr},
        {"/api/auth",                HTTP_GET,      handle_auth_get,       nullptr},
        {"/api/auth",                HTTP_POST,     handle_auth_post,      nullptr},
        {"/api/events",              HTTP_GET,      handle_events_get,     nullptr},
        {"/api/events",              HTTP_DELETE,   handle_events_clear,   nullptr},
        {"/api/logs",                HTTP_GET,      handle_logs_get,       nullptr},
        {"/api/logs",                HTTP_DELETE,   handle_logs_clear,     nullptr},
        {"/api/history",             HTTP_GET,      handle_history_get,    nullptr},
        {"/api/info",                HTTP_GET,      handle_info,           nullptr},
        {"/api/tokens",              HTTP_GET,      handle_tokens_get,     nullptr},
        {"/api/tokens",              HTTP_POST,     handle_tokens_post,    nullptr},
        {"/api/tokens",              HTTP_DELETE,   handle_tokens_delete,  nullptr},
        {"/api/*",                   HTTP_OPTIONS, handle_options,        nullptr},
    };
    static_assert(sizeof(uris) / sizeof(uris[0]) <= 70, "raise max_uri_handlers");
    for (const auto& u : uris) {
        if (httpd_register_uri_handler(s_server, &u) != ESP_OK)
            ESP_LOGE(TAG, "failed to register %s", u.uri);
    }

    ESP_LOGI(TAG, "web server started — http://%s/", wifi::get_ip());

    // Phase 2b: background peer-state poller feeding the conflict model.
    s_peer_mtx = xSemaphoreCreateMutex();
    s_op_mtx   = xSemaphoreCreateMutex();  // Phase 3: dwell guard for resolutions
    if (s_peer_mtx) {
        xTaskCreate(group_poll_task, "grp_poll", 6144, nullptr, 4, nullptr);
    } else {
        ESP_LOGE(TAG, "group poll mutex alloc failed — peer polling disabled");
    }
    s_house_mtx = xSemaphoreCreateMutex();
    if (s_house_mtx)
        xTaskCreate(house_task, "house", 6144, nullptr, 3, &s_house_task);
    return ESP_OK;
}

}  // namespace web_ui
