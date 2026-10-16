/// @file park_ctl.cpp
/// @brief Firmware glue for the "off at setpoint" feature. See park_ctl.h.

#include "park_ctl.h"
#include "events.h"

#include <mutex>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

namespace park_ctl {
namespace {

const char* TAG = "park";
constexpr const char* kNs = "park";

std::mutex    s_mtx;
park::Machine s_m;
Hooks         s_hooks;
bool          s_saved_managed = false;
bool          s_unit_on = false;

uint32_t now_s() { return static_cast<uint32_t>(esp_timer_get_time() / 1000000); }

void load(park::Config& c, bool& managed) {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t u8 = 0;
    uint32_t u32 = 0;
    if (nvs_get_u8(h, "en", &u8) == ESP_OK) c.enabled = u8 != 0;
    if (nvs_get_u32(h, "dlt", &u32) == ESP_OK) c.delta_c = u32 / 10.0f;  // tenths of °C
    if (nvs_get_u32(h, "moff", &u32) == ESP_OK) c.min_off_s = u32;
    if (nvs_get_u8(h, "pen", &u8) == ESP_OK) c.probe_enabled = u8 != 0;
    if (nvs_get_u32(h, "pint", &u32) == ESP_OK) c.probe_interval_s = u32;
    if (nvs_get_u32(h, "pdur", &u32) == ESP_OK) c.probe_duration_s = u32;
    if (nvs_get_u8(h, "parked", &u8) == ESP_OK) managed = u8 != 0;
    nvs_close(h);
}

void save_config(const park::Config& c) {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "en", c.enabled ? 1 : 0);
    nvs_set_u32(h, "dlt", static_cast<uint32_t>(c.delta_c * 10.0f + 0.5f));
    nvs_set_u32(h, "moff", c.min_off_s);
    nvs_set_u8(h, "pen", c.probe_enabled ? 1 : 0);
    nvs_set_u32(h, "pint", c.probe_interval_s);
    nvs_set_u32(h, "pdur", c.probe_duration_s);
    nvs_commit(h);
    nvs_close(h);
}

// Called with s_mtx held. Persist the managed flag only when it changes so a
// steady state never wears flash.
void persist_managed_locked() {
    bool m = s_m.managed();
    if (m == s_saved_managed) return;
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "parked", m ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
    s_saved_managed = m;
}

struct Outcome {
    park::Action action = park::Action::None;
    const char*  event = nullptr;
    bool         changed = false;
};

// Run `fn` against the machine under the lock, then execute side effects with
// the lock released (set_power/on_change re-enter overlay()).
template <typename F>
void step(F fn) {
    Outcome o;
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        park::State before = s_m.state();
        bool parked_before = s_m.parked();
        o.action = fn(s_m);
        o.event = s_m.take_event();
        o.changed = s_m.state() != before || s_m.parked() != parked_before;
        persist_managed_locked();
        if (o.changed)
            ESP_LOGI(TAG, "%s -> %s (%s)", park::state_str(before),
                     park::state_str(s_m.state()), s_m.reason());
    }
    if (o.action != park::Action::None && s_hooks.set_power)
        s_hooks.set_power(o.action == park::Action::PowerOn);
    if (o.event) events::log(events::Cat::Power, events::Actor::System, o.event);
    if (o.changed && s_hooks.on_change) s_hooks.on_change();
}

}  // namespace

void init(const Hooks& hooks) {
    s_hooks = hooks;
    park::Config c;
    bool managed = false;
    load(c, managed);
    std::lock_guard<std::mutex> lk(s_mtx);
    s_m.configure(c, now_s());
    s_m.restore(managed);
    s_saved_managed = managed;
    ESP_LOGI(TAG, "off-at-setpoint %s%s", s_m.enabled() ? "enabled" : "disabled",
             managed ? " (restoring parked state)" : "");
}

void tick() {
    if (!s_hooks.read) return;
    park::Inputs in = s_hooks.read();
    uint32_t t = now_s();
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        s_unit_on = in.phys_on;
    }
    step([&](park::Machine& m) { return m.tick(in, t); });
}

void set_desired_power(bool on) {
    uint32_t t = now_s();
    step([&](park::Machine& m) { return m.set_desired_power(on, t); });
}

void notify_intent_change() {
    uint32_t t = now_s();
    step([&](park::Machine& m) { return m.notify_intent_change(t); });
}

cn105::Settings overlay(cn105::Settings s) {
    std::lock_guard<std::mutex> lk(s_mtx);
    if (s_m.parked()) s.power = "ON";
    return s;
}

park::Config config() {
    std::lock_guard<std::mutex> lk(s_mtx);
    return s_m.config();
}

park::Config set_config(const park::Config& c) {
    park::Config out = park::sanitize(c);
    save_config(out);
    uint32_t t = now_s();
    step([&](park::Machine& m) { return m.configure(out, t); });
    if (s_hooks.on_change) s_hooks.on_change();  // enabled flag is in the state payload
    return out;
}

Snapshot snapshot() {
    std::lock_guard<std::mutex> lk(s_mtx);
    Snapshot s;
    s.enabled = s_m.enabled();
    s.state = s_m.state();
    s.parked = s_m.parked();
    s.reason = s_m.reason();
    s.next_probe_s = s_m.next_probe_s(now_s());
    s.unit_on = s_unit_on;
    return s;
}

}  // namespace park_ctl
