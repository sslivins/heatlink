/// @file park_ctl.cpp
/// @brief Firmware glue for the "off at setpoint" feature. See park_ctl.h.

#include "park_ctl.h"
#include "events.h"
#include "group_config.h"

#include <cctype>
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
park::Config  s_user;              // this head's own settings (NVS)
std::string   s_shared;            // outdoor-unit config from the group ("" = none)
bool          s_slice_active = false;
bool          s_quiet = false;     // air check running at QUIET; s_saved_fan is the user's
std::string   s_saved_fan;
uint32_t      s_quiet_at = 0;
constexpr const char* kQuietFan = "QUIET";
// Held (Tandem) air check running in FAN mode; s_saved_mode is the user's mode.
bool          s_fanmode = false;
std::string   s_saved_mode;
uint32_t      s_fanmode_at = 0;
// After a check the unit reports FAN until it acks the restored mode; keep
// reporting the user's mode meanwhile so Tandem doesn't see the room drop out.
uint32_t      s_restore_until = 0;
constexpr const char* kFanMode = "FAN";

// The head's settings with the outdoor unit's shared fields applied.
park::Config configured_locked() {
    park::Config c = s_user;
    if (!s_shared.empty()) park::decode_shared(s_shared, c);
    return c;
}

park::Config effective_locked() {
    park::Config c = configured_locked();
    if (s_slice_active) c.enabled = true;
    return c;
}

uint32_t now_s() { return static_cast<uint32_t>(esp_timer_get_time() / 1000000); }

bool show_saved_mode_locked(const std::string& unit_mode) {
    return s_fanmode || (unit_mode == kFanMode && now_s() < s_restore_until);
}

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

void persist_fan_locked() {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return;
    if (s_quiet) nvs_set_str(h, "pfan", s_saved_fan.c_str());
    else nvs_erase_key(h, "pfan");
    nvs_commit(h);
    nvs_close(h);
}

void persist_mode_locked() {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return;
    if (s_fanmode) nvs_set_str(h, "pmode", s_saved_mode.c_str());
    else nvs_erase_key(h, "pmode");
    nvs_commit(h);
    nvs_close(h);
}

// Run the unit in FAN mode exactly while a held (Tandem) air check runs, so the
// compressor never runs the other slice's direction. Called before a power
// command is sent so FAN is in the same CN105 packet as power ON (and the
// user's mode is restored with power OFF). Also after a reboot mid-check
// (s_fanmode restored from NVS).
void sync_mode() {
    if (!s_hooks.set_mode || !s_hooks.get_mode) return;
    const std::string mode = s_hooks.get_mode();  // outside s_mtx (CN105 lock)
    std::string send;
    bool republish = false;
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        const bool want = s_m.checking();
        if (want && !s_fanmode) {
            if (mode.empty() || mode == kFanMode) return;  // not connected yet
            s_saved_mode = mode;
            s_fanmode = true;
            s_fanmode_at = now_s();
            persist_mode_locked();
            send = kFanMode;
            republish = true;
        } else if (!want && s_fanmode) {
            s_fanmode = false;
            s_restore_until = now_s() + park::kCmdSettleS;
            persist_mode_locked();
            send = s_saved_mode;
            republish = true;
        } else if (s_fanmode && now_s() - s_fanmode_at >= park::kCmdSettleS &&
                   !mode.empty() && mode != kFanMode) {
            // Changed on the IR remote mid-check: that's the user's new mode.
            // Keep checking in FAN; the slice controller judges the new mode.
            ESP_LOGI(TAG, "mode changed to %s during air check", mode.c_str());
            s_saved_mode = mode;
            s_fanmode_at = now_s();
            persist_mode_locked();
            send = kFanMode;
            republish = true;
        }
    }
    if (!send.empty()) {
        ESP_LOGI(TAG, "held air check mode -> %s", send.c_str());
        s_hooks.set_mode(send);
    }
    if (republish && s_hooks.on_change) s_hooks.on_change();
}

// Keep the fan at QUIET exactly while probing. Runs after every step (and
// after a reboot mid-probe, where s_quiet was restored from NVS).
void sync_fan() {
    if (!s_hooks.set_fan || !s_hooks.get_fan) return;
    const std::string fan = s_hooks.get_fan();  // outside s_mtx (CN105 lock)
    std::string send;
    bool republish = false;
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        const bool probing = s_m.enabled() && s_m.state() == park::State::Probing;
        if (probing && !s_quiet) {
            if (fan.empty()) return;  // not connected yet
            s_saved_fan = fan;
            s_quiet = true;
            s_quiet_at = now_s();
            persist_fan_locked();
            if (fan != kQuietFan) send = kQuietFan;
            republish = true;
        } else if (!probing && s_quiet) {
            s_quiet = false;
            persist_fan_locked();
            if (s_saved_fan != kQuietFan) send = s_saved_fan;
            republish = true;
        } else if (s_quiet && now_s() - s_quiet_at >= park::kCmdSettleS) {
            // Changed on the IR remote mid-check: that's the user's new choice.
            if (!fan.empty() && fan != kQuietFan) {
                ESP_LOGI(TAG, "fan changed to %s during air check; keeping it", fan.c_str());
                s_quiet = false;
                persist_fan_locked();
                republish = true;
            }
        }
    }
    if (!send.empty()) {
        ESP_LOGI(TAG, "air check fan -> %s", send.c_str());
        s_hooks.set_fan(send);
    }
    if (republish && s_hooks.on_change) s_hooks.on_change();
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
    sync_mode();
    if (o.action != park::Action::None && s_hooks.set_power)
        s_hooks.set_power(o.action == park::Action::PowerOn);
    if (o.event) events::log(events::Cat::Power, events::Actor::System, o.event);
    if (o.changed && s_hooks.on_change) s_hooks.on_change();
    sync_fan();
}

}  // namespace

void init(const Hooks& hooks) {
    s_hooks = hooks;
    park::Config c;
    bool managed = false;
    load(c, managed);
    std::lock_guard<std::mutex> lk(s_mtx);
    s_user = park::sanitize(c);
    s_m.configure(s_user, now_s());
    s_m.restore(managed);
    s_saved_managed = managed;
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READONLY, &h) == ESP_OK) {
        char buf[16];
        size_t len = sizeof(buf);
        if (nvs_get_str(h, "pfan", buf, &len) == ESP_OK && buf[0]) {
            s_saved_fan = buf;
            s_quiet = true;  // rebooted mid air check: restored on the first tick
        }
        len = sizeof(buf);
        if (nvs_get_str(h, "pmode", buf, &len) == ESP_OK && buf[0]) {
            s_saved_mode = buf;
            s_fanmode = true;  // rebooted mid held air check: restored on the first tick
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "off-at-setpoint %s%s", s_m.enabled() ? "enabled" : "disabled",
             managed ? " (restoring parked state)" : "");
}

// Adopt the group's shared config when it changes (another head saved it, or
// we joined/left a group).
void sync_shared() {
    std::string enc = hvac_group::park_config();
    park::Config probe;
    if (!enc.empty() && !park::decode_shared(enc, probe)) enc.clear();
    bool was = false, now = false;
    bool changed = false;
    park::Config adopted;
    uint32_t t = now_s();
    step([&](park::Machine& m) {
        if (enc == s_shared) return park::Action::None;
        changed = true;
        was = configured_locked().enabled;
        s_shared = enc;
        // Keep the adopted settings as our own so leaving the group changes nothing.
        s_user = adopted = configured_locked();
        now = adopted.enabled;
        return m.configure(effective_locked(), t);
    });
    if (!changed) return;
    if (!enc.empty()) save_config(adopted);
    ESP_LOGI(TAG, "outdoor-unit config %s", enc.empty() ? "cleared" : enc.c_str());
    if (!enc.empty() && was != now)
        events::log(events::Cat::Power, events::Actor::System,
                    now ? "Off at setpoint enabled for the outdoor unit"
                        : "Off at setpoint disabled for the outdoor unit");
    if (s_hooks.on_change) s_hooks.on_change();
}

void tick() {
    sync_shared();
    if (!s_hooks.read) return;
    park::Inputs in = s_hooks.read();
    uint32_t t = now_s();
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        s_unit_on = in.phys_on;
        if (show_saved_mode_locked(in.mode)) in.mode = s_saved_mode;  // judge the room by the user's mode
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
    // Probing included: the unit reads OFF until CN105 acks the power-on.
    if (s_m.managed()) s.power = "ON";
    if (s_quiet) s.fan = s_saved_fan;
    if (show_saved_mode_locked(s.mode)) s.mode = s_saved_mode;
    return s;
}

bool request_mode(const std::string& mode) {
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        if (!s_fanmode) return false;
        std::string m = mode;
        for (auto& c : m) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        s_saved_mode = m;
        persist_mode_locked();
    }
    if (s_hooks.on_change) s_hooks.on_change();
    return true;
}

bool request_fan(const std::string& fan) {
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        if (!s_quiet) return false;
        s_saved_fan = fan;
        persist_fan_locked();
    }
    if (s_hooks.on_change) s_hooks.on_change();
    return true;
}

park::Config config() {
    std::lock_guard<std::mutex> lk(s_mtx);
    return configured_locked();
}

bool shared() {
    std::lock_guard<std::mutex> lk(s_mtx);
    return !s_shared.empty();
}

park::Config set_config(const park::Config& c) {
    park::Config out = park::sanitize(c);
    save_config(out);
    // Grouped: the enable / min-off / probe settings belong to the outdoor unit.
    std::string enc;
    if (hvac_group::in_group()) {
        enc = park::encode_shared(out);
        if (hvac_group::set_park_config(enc) != ESP_OK) enc.clear();
    }
    uint32_t t = now_s();
    step([&](park::Machine& m) {
        s_user = out;
        if (!enc.empty()) s_shared = enc;
        return m.configure(effective_locked(), t);
    });
    if (s_hooks.on_change) s_hooks.on_change();  // enabled flag is in the state payload
    return out;
}

void set_slice(bool active, bool hold, bool check) {
    uint32_t t = now_s();
    bool republish = false;
    step([&](park::Machine& m) {
        park::Action a = park::Action::None;
        if (active != s_slice_active) {
            s_slice_active = active;
            republish = true;
            a = m.configure(effective_locked(), t);
        }
        // After (re)enable the hold takes effect on the machine's first tick
        // (next CN105 loop pass, <= ~1 s), once it has fresh inputs.
        park::Action h = m.set_hold(active && hold, t);
        if (h != park::Action::None) return h;
        if (a != park::Action::None) return a;
        if (active && hold && check) {
            park::State before = m.state();
            park::Action c = m.start_check(t);
            if (m.state() != before) republish = true;
            return c;
        }
        return park::Action::None;
    });
    if (republish && s_hooks.on_change) s_hooks.on_change();
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
    s.satisfied = s_m.parked_satisfied();
    s.held = s_m.enabled() && s_m.hold();
    s.forced = s_slice_active && !s_user.enabled;
    s.slicing = s_slice_active;
    s.quiet_fan = s_quiet;
    s.need = s_m.need(now_s());
    s.checking = s_m.checking();
    return s;
}

}  // namespace park_ctl
