/// @file recovery.cpp
/// @brief Admin-password reset procedure — see recovery.h.
#include "recovery.h"

#include <mutex>

#include "auth_manager.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "events.h"
#include "nvs.h"

namespace recovery {
namespace {

const char* TAG = "recovery";
const char* kNs = "recov";
const char* kKeyCuts = "cuts";

constexpr uint8_t kCutsNeeded = 5;
// A cut must follow the previous one within this long, else the count resets.
constexpr uint64_t kCutGapUs = 30ULL * 1000 * 1000;

std::mutex g_mtx;
uint8_t g_cuts = 0;
esp_timer_handle_t g_reset_timer = nullptr;

void save_cuts() {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, kKeyCuts, g_cuts);
    nvs_commit(h);
    nvs_close(h);
}

void on_gap_expired(void*) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_cuts == 0) return;
    ESP_LOGI(TAG, "no further power cut within 30 s — sequence reset (%u)", g_cuts);
    g_cuts = 0;
    save_cuts();
}

void arm_gap_timer() {
    if (!g_reset_timer) {
        esp_timer_create_args_t a = {};
        a.callback = &on_gap_expired;
        a.dispatch_method = ESP_TIMER_TASK;
        a.name = "recov_gap";
        if (esp_timer_create(&a, &g_reset_timer) != ESP_OK) return;
    }
    esp_timer_stop(g_reset_timer);
    esp_timer_start_once(g_reset_timer, kCutGapUs);
}

// Record one power cut (g_mtx held). Returns true when the sequence completes.
bool count_cut_locked() {
    if (g_cuts < 0xFF) g_cuts++;
    ESP_LOGI(TAG, "power cut %u/%u", g_cuts, kCutsNeeded);
    if (g_cuts >= kCutsNeeded) {
        g_cuts = 0;
        save_cuts();
        if (g_reset_timer) esp_timer_stop(g_reset_timer);
        return true;
    }
    save_cuts();
    arm_gap_timer();
    return false;
}

}  // namespace

void init() {
    bool fire = false;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        nvs_handle_t h;
        if (nvs_open(kNs, NVS_READONLY, &h) == ESP_OK) {
            nvs_get_u8(h, kKeyCuts, &g_cuts);
            nvs_close(h);
        }
        esp_reset_reason_t r = esp_reset_reason();
        if (r == ESP_RST_POWERON || r == ESP_RST_BROWNOUT) {
            fire = count_cut_locked();
        } else if (g_cuts != 0) {
            g_cuts = 0;  // any other reset (OTA, crash, restart) breaks the sequence
            save_cuts();
        }
    }
    if (fire) trigger(false);
}

void note_power_restored() {
    bool fire;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        fire = count_cut_locked();
    }
    if (fire) trigger(false);
}

void trigger(bool from_ha) {
    auth_mgr_init();  // idempotent; recovery can run before the web server starts
    auth_mgr_recover();
    events::log(events::Cat::System,
                from_ha ? events::Actor::HomeAssistant : events::Actor::Local,
                from_ha ? "Admin password reset procedure run from Home Assistant"
                        : "Admin password reset procedure run (power cycles)");
}

}  // namespace recovery
