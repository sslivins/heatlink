/// @file room_sensor_ctl.cpp
/// @brief Wireless room-sensor glue (#142). See room_sensor_ctl.h.

#include "room_sensor_ctl.h"
#include "events.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"

namespace room_sensor_ctl {
namespace {

using room_sensor::Kind;
using room_sensor::Sensor;
using room_sensor::SensorSet;

const char* TAG = "rsensor";
constexpr const char* kNs = "rsensor";

// Remote temperature is resent this often even when unchanged, so the unit
// never drops back to its own sensor while ours are fresh.
constexpr uint32_t kResendS = 120;
constexpr uint32_t kWifiPairWindowS = 180;
constexpr uint32_t kDiscoverEveryS = 8;
constexpr uint32_t kSyncRetryS = 60;

// Passive scan duty (0.625 ms units). Wi-Fi shares the radio, so idle
// listening is light; the pairing window listens harder to catch the press.
constexpr uint16_t kIdleItvl = 320, kIdleWindow = 80;   // 200 ms / 50 ms (25 %)
constexpr uint16_t kPairItvl = 160, kPairWindow = 96;   // 100 ms / 60 ms (60 %)

std::mutex  s_mtx;
SensorSet   s_set;
Hooks       s_hooks;
bool        s_dirty = false;               // persist the sensor list
std::vector<std::string> s_events;         // log outside the lock
std::vector<Sensor> s_unhook;              // removed Shelly sensors to clean up
bool        s_kick = false;                // a Shelly just reported: it's awake
uint32_t    s_last_sync_try = 0;
// Wi-Fi pairing.
uint32_t    s_wifi_until = 0;
uint32_t    s_last_discover = 0;
std::vector<shelly_rpc::Found> s_found;
std::string s_pair_host;                   // queued pair request
std::string s_pair_state = "idle";
std::string s_pair_msg;
// Remote-temperature feed (worker only, s_feeding also read elsewhere).
std::atomic<bool> s_feeding{false};
float       s_sent = 0.0f;
uint32_t    s_sent_at = 0;
bool        s_lost_logged = false;

// BLE state (touched by the worker task and the NimBLE host task).
volatile bool s_ble_started = false;
volatile bool s_synced = false;
volatile bool s_scanning = false;
bool          s_scan_fast = false;
uint8_t       s_own_addr_type = 0;
std::string   s_ble_error;
std::atomic<uint32_t> s_adverts{0};

uint32_t now_s() { return static_cast<uint32_t>(esp_timer_get_time() / 1000000); }

std::string label(const Sensor& s) { return s.name.empty() ? s.id : s.name; }

// ── NVS: slot i keys id<i> kd<i> nm<i> ho<i> tk<i> hi<i> st<i> pd<i> ───────
std::string key(const char* k, size_t i) { return std::string(k) + std::to_string(i); }

std::string get_str(nvs_handle_t h, const std::string& k) {
    char buf[72] = {};
    size_t len = sizeof(buf);
    return nvs_get_str(h, k.c_str(), buf, &len) == ESP_OK ? std::string(buf) : std::string();
}

void load() {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) return;
    for (size_t i = 0; i < room_sensor::kMaxSensors; ++i) {
        Sensor s;
        s.id = get_str(h, key("id", i));
        if (s.id.empty()) continue;
        uint8_t kd = 0, st = 0, pd = 0;
        nvs_get_u8(h, key("kd", i).c_str(), &kd);
        s.kind = kd == static_cast<uint8_t>(Kind::ShellyRpc) ? Kind::ShellyRpc : Kind::BTHome;
        s.name = get_str(h, key("nm", i));
        s.host = get_str(h, key("ho", i));
        s.token = get_str(h, key("tk", i));
        s.hook_ip = get_str(h, key("hi", i));
        if (nvs_get_u8(h, key("st", i).c_str(), &st) == ESP_OK && st > 0)
            s.step_c = shelly::clamp_step(st / 10.0f);
        if (nvs_get_u8(h, key("pd", i).c_str(), &pd) == ESP_OK) s.sync_pending = pd != 0;
        s_set.add(s);
    }
    nvs_close(h);
}

void save(const std::vector<Sensor>& list) {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_all(h);
    for (size_t i = 0; i < list.size(); ++i) {
        const Sensor& s = list[i];
        nvs_set_str(h, key("id", i).c_str(), s.id.c_str());
        nvs_set_u8(h, key("kd", i).c_str(), static_cast<uint8_t>(s.kind));
        nvs_set_str(h, key("nm", i).c_str(), s.name.substr(0, 63).c_str());
        if (s.kind == Kind::ShellyRpc) {
            nvs_set_str(h, key("ho", i).c_str(), s.host.c_str());
            nvs_set_str(h, key("tk", i).c_str(), s.token.c_str());
            nvs_set_str(h, key("hi", i).c_str(), s.hook_ip.c_str());
            nvs_set_u8(h, key("st", i).c_str(),
                       static_cast<uint8_t>(std::lround(s.step_c * 10.0f)));
            nvs_set_u8(h, key("pd", i).c_str(), s.sync_pending ? 1 : 0);
        }
    }
    nvs_commit(h);
    nvs_close(h);
}

// ── Bluetooth (BTHome) ───────────────────────────────────────────────────
int gap_event(struct ble_gap_event* ev, void*) {
    switch (ev->type) {
        case BLE_GAP_EVENT_DISC: {
            s_adverts.fetch_add(1, std::memory_order_relaxed);
            bthome::Reading r;
            if (!bthome::parse(ev->disc.data, ev->disc.length_data, r)) return 0;
            const std::string addr = bthome::addr_str(ev->disc.addr.val);
            std::lock_guard<std::mutex> lk(s_mtx);
            const auto res = s_set.on_advert(addr, ev->disc.rssi, r, now_s());
            if (res == SensorSet::Result::Paired) {
                s_dirty = true;
                const Sensor* s = s_set.find(addr);
                s_events.push_back("Room sensor added: " + (s ? label(*s) : addr));
            }
            return 0;
        }
        case BLE_GAP_EVENT_DISC_COMPLETE:
            s_scanning = false;
            return 0;
        default:
            return 0;
    }
}

void on_sync() {
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) ESP_LOGW(TAG, "no BLE address (%d)", rc);
    s_synced = rc == 0;
}

void on_reset(int reason) {
    ESP_LOGW(TAG, "BLE host reset (%d)", reason);
    s_synced = false;
    s_scanning = false;
}

void host_task(void*) {
    nimble_port_run();  // returns only on nimble_port_stop()
    nimble_port_freertos_deinit();
}

void ble_start() {
    if (s_ble_started) return;
    s_ble_started = true;  // one attempt per boot
    esp_err_t e = nimble_port_init();
    if (e != ESP_OK) {
        s_ble_error = esp_err_to_name(e);
        ESP_LOGE(TAG, "nimble_port_init failed: %s", s_ble_error.c_str());
        return;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "BLE started");
}

void scan_set(bool want, bool fast) {
    if (!s_synced) return;
    if (!want) {
        if (s_scanning) {
            ble_gap_disc_cancel();
            s_scanning = false;
            ESP_LOGI(TAG, "scan stopped");
        }
        return;
    }
    if (s_scanning && fast == s_scan_fast) return;
    if (s_scanning) ble_gap_disc_cancel();
    struct ble_gap_disc_params p = {};
    p.itvl = fast ? kPairItvl : kIdleItvl;
    p.window = fast ? kPairWindow : kIdleWindow;
    p.passive = 1;
    p.filter_duplicates = 0;  // every advert carries a fresh reading
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &p, gap_event, nullptr);
    s_scanning = rc == 0;
    s_scan_fast = fast;
    if (rc != 0) ESP_LOGW(TAG, "ble_gap_disc failed (%d)", rc);
    else ESP_LOGI(TAG, "scanning (%s)", fast ? "pairing" : "idle");
}

// ── Shelly (Wi-Fi) ───────────────────────────────────────────────────────
std::string new_token() {
    char b[33];
    for (int i = 0; i < 4; ++i) snprintf(b + i * 8, 9, "%08lx", (unsigned long)esp_random());
    return b;
}

cJSON* id_param(int id) {
    cJSON* p = cJSON_CreateObject();
    cJSON_AddNumberToObject(p, "id", id);
    return p;
}

const cJSON* path(const cJSON* o, std::initializer_list<const char*> keys) {
    for (const char* k : keys) {
        if (!o) return nullptr;
        o = cJSON_GetObjectItem(o, k);
    }
    return o;
}

/// (Re)install our webhooks and the report step on an awake sensor. Updates
/// hook_ip / sync_pending / sync_error. False if the sensor wasn't reachable
/// or refused the hooks.
bool shelly_sync(Sensor& s, int timeout_ms) {
    const std::string ip = shelly_rpc::own_ip();
    if (ip.empty()) {
        s.sync_error = "head has no IP";
        return false;
    }
    std::string err;
    cJSON* list = nullptr;
    if (!shelly_rpc::call(s.host, "Webhook.List", nullptr, &list, &err, timeout_ms)) {
        s.sync_error = err;
        return false;
    }
    // Drop every earlier HeatLink hook (ours or another head's).
    const cJSON* hooks = cJSON_GetObjectItem(list, "hooks");
    const cJSON* hk = nullptr;
    cJSON_ArrayForEach(hk, hooks) {
        const cJSON* nm = cJSON_GetObjectItem(hk, "name");
        const cJSON* id = cJSON_GetObjectItem(hk, "id");
        bool ours = nm && cJSON_IsString(nm) && std::string(nm->valuestring) == shelly::kHookName;
        if (!ours) {
            const cJSON* u = nullptr;
            cJSON_ArrayForEach(u, cJSON_GetObjectItem(hk, "urls")) {
                if (cJSON_IsString(u) && strstr(u->valuestring, shelly::kReportPath)) ours = true;
            }
        }
        if (ours && id && cJSON_IsNumber(id))
            shelly_rpc::call(s.host, "Webhook.Delete", id_param(id->valueint), nullptr, nullptr,
                             timeout_ms);
    }
    cJSON_Delete(list);

    const std::string url = shelly::report_url(ip, s.token);
    bool temp_ok = false;
    for (const char* ev : {"temperature.change", "humidity.change"}) {
        cJSON* p = cJSON_CreateObject();
        cJSON_AddNumberToObject(p, "cid", 0);
        cJSON_AddBoolToObject(p, "enable", true);
        cJSON_AddStringToObject(p, "event", ev);
        cJSON_AddStringToObject(p, "name", shelly::kHookName);
        cJSON* urls = cJSON_AddArrayToObject(p, "urls");
        cJSON_AddItemToArray(urls, cJSON_CreateString(url.c_str()));
        std::string e;
        bool ok = shelly_rpc::call(s.host, "Webhook.Create", p, nullptr, &e, timeout_ms);
        if (std::string(ev) == "temperature.change") {
            temp_ok = ok;
            if (!ok) err = e;
        }
    }
    if (!temp_ok) {
        s.sync_error = "webhook: " + err;
        return false;
    }
    s.hook_ip = ip;
    s.sync_pending = false;
    s.sync_error.clear();

    cJSON* p = id_param(0);
    cJSON* cfg = cJSON_AddObjectToObject(p, "config");
    cJSON_AddNumberToObject(cfg, "report_thr_C", s.step_c);
    if (!shelly_rpc::call(s.host, "Temperature.SetConfig", p, nullptr, &err, timeout_ms)) {
        // A refused value won't succeed on retry: report it, don't keep retrying.
        s.sync_error = "report step: " + err;
    }
    return true;
}

void pair_shelly(const std::string& host) {
    auto fail = [&](const std::string& m) {
        std::lock_guard<std::mutex> lk(s_mtx);
        s_pair_state = "error";
        s_pair_msg = m;
        ESP_LOGW(TAG, "pair %s: %s", host.c_str(), m.c_str());
    };
    std::string err;
    cJSON* info = nullptr;
    if (!shelly_rpc::call(host, "Shelly.GetDeviceInfo", nullptr, &info, &err)) return fail(err);
    const cJSON* jid = cJSON_GetObjectItem(info, "id");
    const cJSON* jnm = cJSON_GetObjectItem(info, "name");
    const cJSON* japp = cJSON_GetObjectItem(info, "app");
    Sensor s;
    s.kind = Kind::ShellyRpc;
    s.host = host;
    s.id = (jid && cJSON_IsString(jid)) ? jid->valuestring : "";
    if (jnm && cJSON_IsString(jnm)) s.name = jnm->valuestring;
    else if (japp && cJSON_IsString(japp)) s.name = std::string("Shelly ") + japp->valuestring;
    cJSON_Delete(info);
    if (s.id.empty()) return fail("not a Shelly Gen2+ device");

    cJSON* st = nullptr;
    if (!shelly_rpc::call(host, "Shelly.GetStatus", nullptr, &st, &err)) return fail(err);
    const cJSON* tc = path(st, {"temperature:0", "tC"});
    if (!cJSON_GetObjectItem(st, "temperature:0")) {
        cJSON_Delete(st);
        return fail("this device has no temperature sensor");
    }
    const uint32_t t = now_s();
    if (tc && cJSON_IsNumber(tc)) {
        s.sample.valid = true;
        s.sample.temp_c = static_cast<float>(tc->valuedouble);
        s.sample.temp_at_s = t;
    }
    const cJSON* rh = path(st, {"humidity:0", "rh"});
    if (rh && cJSON_IsNumber(rh)) {
        s.sample.has_humidity = true;
        s.sample.humidity = static_cast<float>(rh->valuedouble);
    }
    const cJSON* bp = path(st, {"devicepower:0", "battery", "percent"});
    if (bp && cJSON_IsNumber(bp)) {
        s.sample.has_battery = true;
        s.sample.battery = static_cast<uint8_t>(bp->valueint);
    }
    const cJSON* rs = path(st, {"wifi", "rssi"});
    if (rs && cJSON_IsNumber(rs)) {
        s.sample.has_rssi = true;
        s.sample.rssi = rs->valueint;
    }
    s.sample.seen = true;
    s.sample.seen_at_s = t;
    cJSON_Delete(st);

    {
        std::lock_guard<std::mutex> lk(s_mtx);
        const Sensor* old = s_set.find(s.id);
        if (!old && s_set.full()) {
            s_pair_state = "error";
            s_pair_msg = "full";
            return;
        }
        if (old) s.step_c = old->step_c;
    }
    s.token = new_token();
    if (!shelly_sync(s, 4000)) return fail(s.sync_error);

    std::lock_guard<std::mutex> lk(s_mtx);
    if (!s_set.find(s.id) && s_set.full()) {
        s_pair_state = "error";
        s_pair_msg = "full";
        return;
    }
    s_set.add(s);
    s_dirty = true;
    s_events.push_back("Room sensor added: " + label(s));
    s_pair_state = "ok";
    s_pair_msg = label(s);
    s_wifi_until = 0;
    ESP_LOGI(TAG, "paired Shelly %s at %s", s.id.c_str(), host.c_str());
}

/// Retry pending hook/step updates on Shelly sensors (or after a head IP change).
void shelly_maintain(uint32_t t) {
    bool kick;
    std::vector<Sensor> todo;
    const std::string ip = shelly_rpc::own_ip();
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        kick = s_kick;
        s_kick = false;
        if (!kick && t - s_last_sync_try < kSyncRetryS) return;
        for (const Sensor& s : s_set.sensors()) {
            if (s.kind != Kind::ShellyRpc || s.host.empty()) continue;
            if (s.sync_pending || (!ip.empty() && s.hook_ip != ip)) todo.push_back(s);
        }
        if (todo.empty()) return;
        s_last_sync_try = t;
    }
    for (Sensor& s : todo) {
        const bool ok = shelly_sync(s, kick ? 2500 : 3000);
        std::lock_guard<std::mutex> lk(s_mtx);
        Sensor* cur = s_set.find(s.id);
        if (!cur || cur->token != s.token) continue;  // removed / re-paired meanwhile
        cur->sync_error = s.sync_error;
        if (ok) {
            cur->hook_ip = s.hook_ip;
            cur->sync_pending = false;
            s_dirty = true;
            ESP_LOGI(TAG, "updated Shelly %s", s.id.c_str());
        }
    }
}

/// Best effort: remove our hooks from a sensor that was unpaired (it is
/// usually asleep, so this often fails; stray reports are then rejected).
void shelly_unhook(const Sensor& s) {
    cJSON* list = nullptr;
    if (!shelly_rpc::call(s.host, "Webhook.List", nullptr, &list, nullptr, 2500)) return;
    const cJSON* hk = nullptr;
    cJSON_ArrayForEach(hk, cJSON_GetObjectItem(list, "hooks")) {
        const cJSON* u = nullptr;
        bool mine = false;
        cJSON_ArrayForEach(u, cJSON_GetObjectItem(hk, "urls")) {
            if (cJSON_IsString(u) && strstr(u->valuestring, s.token.c_str())) mine = true;
        }
        const cJSON* id = cJSON_GetObjectItem(hk, "id");
        if (mine && id && cJSON_IsNumber(id))
            shelly_rpc::call(s.host, "Webhook.Delete", id_param(id->valueint), nullptr, nullptr,
                             2500);
    }
    cJSON_Delete(list);
}

float snap(float c) { return std::round(c * 2.0f) / 2.0f; }

void feed(uint32_t t) {
    float temp = 0.0f;
    size_t n = 0;
    bool any, had_any;
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        any = s_set.room_temp(t, &temp, &n);
        had_any = !s_set.sensors().empty();
    }
    bool changed = false;
    if (any) {
        if (!s_feeding || snap(temp) != snap(s_sent) || t - s_sent_at >= kResendS) {
            if (!s_feeding) {
                ESP_LOGI(TAG, "room temperature from %u sensor(s): %.2f", (unsigned)n, temp);
                if (s_lost_logged)
                    events::log(events::Cat::System, events::Actor::System,
                                "Room sensors back \xE2\x80\x94 using their temperature");
                s_lost_logged = false;
                changed = true;
            }
            if (s_hooks.set_remote) s_hooks.set_remote(temp);
            s_feeding = true;
            s_sent = temp;
            s_sent_at = t;
        }
    } else if (s_feeding) {
        if (s_hooks.set_remote) s_hooks.set_remote(0.0f);  // back to the unit's sensor
        s_feeding = false;
        changed = true;
        ESP_LOGW(TAG, "no fresh room sensor; using the unit's own sensor");
        if (had_any && !s_lost_logged) {
            events::log(events::Cat::System, events::Actor::System,
                        "Room sensors not heard \xE2\x80\x94 using the unit's own sensor");
            s_lost_logged = true;
        }
    }
    if (changed && s_hooks.on_change) s_hooks.on_change();
}

void worker(void*) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        const uint32_t t = now_s();
        bool want, fast, save_now = false, discover = false;
        std::string pair_host;
        std::vector<Sensor> list, unhook;
        std::vector<std::string> evs;
        {
            std::lock_guard<std::mutex> lk(s_mtx);
            want = s_set.wants_scan(t);
            fast = s_set.pairing(t);
            if (s_dirty) {
                s_dirty = false;
                save_now = true;
                list = s_set.sensors();
            }
            evs.swap(s_events);
            unhook.swap(s_unhook);
            pair_host.swap(s_pair_host);
            if (s_wifi_until && t >= s_wifi_until) {
                s_wifi_until = 0;
                s_found.clear();
            }
            discover = s_wifi_until && pair_host.empty() && t - s_last_discover >= kDiscoverEveryS;
        }
        if (save_now) save(list);
        for (const auto& e : evs) {
            ESP_LOGI(TAG, "%s", e.c_str());
            events::log(events::Cat::System, events::Actor::System, e);
        }

        if (want) ble_start();
        scan_set(want, fast);

        if (!pair_host.empty()) pair_shelly(pair_host);
        if (discover) {
            auto found = shelly_rpc::discover(2000);
            std::lock_guard<std::mutex> lk(s_mtx);
            s_last_discover = now_s();
            if (s_wifi_until) {
                // Hide sensors this head already has.
                s_found.clear();
                for (auto& f : found) {
                    bool have = false;
                    for (const Sensor& s : s_set.sensors()) have |= s.host == f.ip;
                    if (!have) s_found.push_back(f);
                }
            }
        }
        for (const Sensor& s : unhook) shelly_unhook(s);
        shelly_maintain(now_s());
        feed(now_s());
    }
}

}  // namespace

void init(const Hooks& hooks) {
    s_hooks = hooks;
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        load();
        ESP_LOGI(TAG, "%u room sensor(s)", (unsigned)s_set.sensors().size());
    }
    xTaskCreate(worker, "rsensor", 6144, nullptr, 3, nullptr);
}

bool fresh_temp(float* out) {
    std::lock_guard<std::mutex> lk(s_mtx);
    return s_set.room_temp(now_s(), out);
}

bool active() { return s_feeding; }

Snapshot snapshot() {
    const uint32_t t = now_s();
    Snapshot o;
    std::lock_guard<std::mutex> lk(s_mtx);
    for (const Sensor& s : s_set.sensors()) {
        SensorView v;
        v.s = s;
        v.s.token.clear();  // never leaves the head
        v.fresh = SensorSet::fresh(s, t);
        if (s.sample.valid) v.temp_age_s = static_cast<int32_t>(t - s.sample.temp_at_s);
        if (s.sample.seen) v.seen_age_s = static_cast<int32_t>(t - s.sample.seen_at_s);
        v.stale_s = room_sensor::stale_s(s.kind);
        o.sensors.push_back(v);
    }
    o.has_temp = s_set.room_temp(t, &o.temp, &o.n_fresh);
    o.ble_pairing = s_set.pairing(t);
    o.ble_remaining_s = s_set.pairing_remaining_s(t);
    o.nearby = s_set.nearby(t);
    for (const auto& n : o.nearby) o.nearby_age_s.push_back(static_cast<int32_t>(t - n.seen_at_s));
    o.wifi_pairing = s_wifi_until > t;
    o.wifi_remaining_s = o.wifi_pairing ? s_wifi_until - t : 0;
    o.found = s_found;
    o.pair_state = s_pair_state;
    o.pair_message = s_pair_msg;
    o.ble_scanning = s_scanning;
    o.ble_error = s_ble_error;
    o.ble_adverts = s_adverts.load(std::memory_order_relaxed);
    return o;
}

Res start_ble_pairing() {
    std::lock_guard<std::mutex> lk(s_mtx);
    if (s_set.full()) return Res::Full;
    s_wifi_until = 0;
    s_set.start_pairing(now_s());
    return Res::Ok;
}

Res start_wifi_pairing() {
    std::lock_guard<std::mutex> lk(s_mtx);
    if (s_set.full()) return Res::Full;
    s_set.cancel_pairing();
    const uint32_t t = now_s();
    s_wifi_until = t + kWifiPairWindowS;
    s_last_discover = t - kDiscoverEveryS;  // look right away
    s_found.clear();
    s_pair_state = "idle";
    s_pair_msg.clear();
    return Res::Ok;
}

void cancel_pairing() {
    std::lock_guard<std::mutex> lk(s_mtx);
    s_set.cancel_pairing();
    s_wifi_until = 0;
    s_found.clear();
    if (s_pair_state != "working") {
        s_pair_state = "idle";
        s_pair_msg.clear();
    }
}

Res select(const std::string& addr) {
    std::string a = bthome::normalize_addr(addr);
    if (a.empty()) return Res::Invalid;
    std::lock_guard<std::mutex> lk(s_mtx);
    switch (s_set.select(a, now_s())) {
        case SensorSet::Result::Paired: {
            s_dirty = true;
            const Sensor* s = s_set.find(a);
            s_events.push_back("Room sensor added: " + (s ? label(*s) : a));
            return Res::Ok;
        }
        case SensorSet::Result::Full: return Res::Full;
        default: return Res::NotFound;
    }
}

Res pair_wifi(const std::string& host) {
    if (host.empty() || host.size() > 63) return Res::Invalid;
    for (char c : host)
        if (!(isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-')) return Res::Invalid;
    std::lock_guard<std::mutex> lk(s_mtx);
    if (s_pair_state == "working" || !s_pair_host.empty()) return Res::Busy;
    s_pair_host = host;
    s_pair_state = "working";
    s_pair_msg.clear();
    return Res::Ok;
}

Res remove(const std::string& id) {
    std::lock_guard<std::mutex> lk(s_mtx);
    const Sensor* s = s_set.find(id);
    if (!s) return Res::NotFound;
    if (s->kind == Kind::ShellyRpc) s_unhook.push_back(*s);
    s_events.push_back("Room sensor removed: " + label(*s));
    s_set.remove(id);
    s_dirty = true;
    return Res::Ok;
}

Res configure(const std::string& id, const std::string* name, float step_c) {
    std::lock_guard<std::mutex> lk(s_mtx);
    Sensor* s = s_set.find(id);
    if (!s) return Res::NotFound;
    if (name) s->name = name->substr(0, 63);
    if (step_c > 0.0f) {
        if (s->kind != Kind::ShellyRpc) return Res::Invalid;
        const float c = shelly::clamp_step(step_c);
        if (c != s->step_c) {
            s->step_c = c;
            s->sync_pending = true;
            s->sync_error.clear();
        }
    }
    s_dirty = true;
    return Res::Ok;
}

bool on_report(const std::string& query, const std::string& peer_ip) {
    shelly::Report r;
    if (!shelly::parse_report(query, r)) return false;
    std::lock_guard<std::mutex> lk(s_mtx);
    std::string id;
    if (s_set.on_report(r, now_s(), &id) != SensorSet::Result::Updated) return false;
    Sensor* s = s_set.find(id);
    if (s && !peer_ip.empty() && s->host != peer_ip) {
        s->host = peer_ip;  // the sensor's DHCP lease moved
        s_dirty = true;
    }
    if (s && (s->sync_pending || s->hook_ip != shelly_rpc::own_ip())) s_kick = true;
    return true;
}

}  // namespace room_sensor_ctl
