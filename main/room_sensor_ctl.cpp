/// @file room_sensor_ctl.cpp
/// @brief Wireless room-sensor glue (#142). See room_sensor_ctl.h.

#include "room_sensor_ctl.h"
#include "events.h"

#include <atomic>
#include <cmath>
#include <mutex>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"

namespace room_sensor_ctl {
namespace {

const char* TAG = "rsensor";
constexpr const char* kNs = "rsensor";

// Remote temperature is resent this often even when unchanged, so the unit
// never drops back to its own sensor while ours is fresh.
constexpr uint32_t kResendS = 120;

// Passive scan duty (0.625 ms units). Wi-Fi shares the radio, so idle
// listening is light; the pairing window listens harder to catch the press.
constexpr uint16_t kIdleItvl = 320, kIdleWindow = 80;   // 200 ms / 50 ms (25 %)
constexpr uint16_t kPairItvl = 160, kPairWindow = 96;   // 100 ms / 60 ms (60 %)

std::mutex           s_mtx;
room_sensor::Tracker s_t;
Hooks                s_hooks;
bool                 s_save = false;     // paired sensor changed: persist + log
bool                 s_feeding = false;  // remote temp is ours
float                s_sent = 0.0f;
uint32_t             s_sent_at = 0;
bool                 s_lost_logged = false;

// BLE state (touched by the worker task and the NimBLE host task).
volatile bool s_ble_started = false;
volatile bool s_synced = false;
volatile bool s_scanning = false;
bool          s_scan_fast = false;
uint8_t       s_own_addr_type = 0;
std::string   s_ble_error;
std::atomic<uint32_t> s_adverts{0};  // every advert heard (diagnostics)

uint32_t now_s() { return static_cast<uint32_t>(esp_timer_get_time() / 1000000); }

void load() {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) return;
    char addr[24] = {}, name[40] = {};
    size_t al = sizeof(addr), nl = sizeof(name);
    if (nvs_get_str(h, "addr", addr, &al) == ESP_OK) {
        if (nvs_get_str(h, "name", name, &nl) != ESP_OK) name[0] = 0;
        std::string a = bthome::normalize_addr(addr);
        if (!a.empty()) s_t.set_paired(a, name);
    }
    nvs_close(h);
}

void save(const std::string& addr, const std::string& name) {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return;
    if (addr.empty()) {
        nvs_erase_all(h);
    } else {
        nvs_set_str(h, "addr", addr.c_str());
        nvs_set_str(h, "name", name.substr(0, 31).c_str());
    }
    nvs_commit(h);
    nvs_close(h);
}

int gap_event(struct ble_gap_event* ev, void*) {
    switch (ev->type) {
        case BLE_GAP_EVENT_DISC: {
            s_adverts.fetch_add(1, std::memory_order_relaxed);
            bthome::Reading r;
            if (!bthome::parse(ev->disc.data, ev->disc.length_data, r)) return 0;
            const std::string addr = bthome::addr_str(ev->disc.addr.val);
            std::lock_guard<std::mutex> lk(s_mtx);
            if (s_t.on_advert(addr, ev->disc.rssi, r, now_s()) ==
                room_sensor::Tracker::Result::Paired)
                s_save = true;
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

float snap(float c) { return std::round(c * 2.0f) / 2.0f; }

void worker(void*) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        const uint32_t t = now_s();
        bool want, fast, fresh, save_now = false, paired;
        float temp = 0.0f;
        std::string addr, name;
        {
            std::lock_guard<std::mutex> lk(s_mtx);
            want = s_t.wants_scan(t);
            fast = s_t.pairing(t);
            fresh = s_t.fresh(t);
            paired = s_t.paired();
            temp = s_t.sample().temp_c;
            if (s_save) {
                s_save = false;
                save_now = true;
                addr = s_t.addr();
                name = s_t.name();
            }
        }
        if (save_now) {
            save(addr, name);
            std::string who = name.empty() ? addr : name + " (" + addr + ")";
            ESP_LOGI(TAG, "%s", addr.empty() ? "unpaired" : ("paired " + who).c_str());
            events::log(events::Cat::System, events::Actor::System,
                        addr.empty() ? "Room sensor removed" : "Room sensor paired: " + who);
            s_lost_logged = false;
        }

        if (want) ble_start();
        scan_set(want, fast);

        bool changed = false;
        if (fresh) {
            if (!s_feeding || snap(temp) != snap(s_sent) || t - s_sent_at >= kResendS) {
                if (!s_feeding) {
                    ESP_LOGI(TAG, "room temperature from sensor: %.2f", temp);
                    if (s_lost_logged)
                        events::log(events::Cat::System, events::Actor::System,
                                    "Room sensor back \xE2\x80\x94 using its temperature");
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
            ESP_LOGW(TAG, "room sensor %s; using the unit's own sensor",
                     paired ? "stale" : "removed");
            if (paired && !s_lost_logged) {
                events::log(events::Cat::System, events::Actor::System,
                            "Room sensor not heard for 10 min \xE2\x80\x94 using the unit's own sensor");
                s_lost_logged = true;
            }
        }
        if (changed && s_hooks.on_change) s_hooks.on_change();
    }
}

}  // namespace

void init(const Hooks& hooks) {
    s_hooks = hooks;
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        load();
        if (s_t.paired()) ESP_LOGI(TAG, "paired sensor %s", s_t.addr().c_str());
    }
    xTaskCreate(worker, "rsensor", 4096, nullptr, 3, nullptr);
}

bool fresh_temp(float* out) {
    std::lock_guard<std::mutex> lk(s_mtx);
    if (!s_t.fresh(now_s())) return false;
    if (out) *out = s_t.sample().temp_c;
    return true;
}

bool active() { return s_feeding; }

Snapshot snapshot() {
    const uint32_t t = now_s();
    Snapshot s;
    std::lock_guard<std::mutex> lk(s_mtx);
    s.paired = s_t.paired();
    s.addr = s_t.addr();
    s.name = s_t.name();
    s.fresh = s_t.fresh(t);
    s.sample = s_t.sample();
    if (s.sample.valid) s.temp_age_s = static_cast<int32_t>(t - s.sample.temp_at_s);
    if (s.sample.seen) s.seen_age_s = static_cast<int32_t>(t - s.sample.seen_at_s);
    s.pairing = s_t.pairing(t);
    s.pairing_remaining_s = s_t.pairing_remaining_s(t);
    s.nearby = s_t.nearby(t);
    for (const auto& n : s.nearby) s.nearby_age_s.push_back(static_cast<int32_t>(t - n.seen_at_s));
    s.ble_ready = s_scanning;
    s.ble_error = s_ble_error;
    s.ble_adverts = s_adverts.load(std::memory_order_relaxed);
    return s;
}

void start_pairing() {
    std::lock_guard<std::mutex> lk(s_mtx);
    s_t.start_pairing(now_s());
}

void cancel_pairing() {
    std::lock_guard<std::mutex> lk(s_mtx);
    s_t.cancel_pairing();
}

bool select(const std::string& addr) {
    std::string a = bthome::normalize_addr(addr);
    if (a.empty()) return false;
    std::lock_guard<std::mutex> lk(s_mtx);
    if (!s_t.select(a, now_s())) return false;
    s_save = true;
    return true;
}

void unpair() {
    std::lock_guard<std::mutex> lk(s_mtx);
    if (!s_t.paired()) return;
    s_t.set_paired("");
    s_save = true;
}

}  // namespace room_sensor_ctl
