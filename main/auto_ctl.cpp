/// @file auto_ctl.cpp
/// @brief See auto_ctl.h.

#include "auto_ctl.h"
#include "events.h"
#include "park_ctl.h"
#include "slice_ctl.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <mutex>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

namespace auto_ctl {
namespace {

using autorange::Dir;
using autorange::Range;

const char* TAG = "auto";
constexpr const char* kNs = "auto";
constexpr uint32_t kCmdSettleS = 15;   ///< our command may take this long to show
constexpr uint32_t kExternalS = 8;     ///< an unexplained change must persist this long
constexpr uint32_t kAdoptS = 3;        ///< IR AUTO: mode + setpoint arrive separately
constexpr uint32_t kSwitchGuardS = 60; ///< ignore readings right after a switch

std::mutex s_mtx;
Hooks      s_hooks;
bool       s_on = false;
Range      s_range;
Dir        s_dir = Dir::Heat;

uint32_t    s_last_seq = 0;
uint32_t    s_switch_at = 0;
bool        s_switched = false;
// Our last command, until the unit reports it (or it times out).
uint32_t    s_cmd_until = 0;
std::string s_cmd_mode;
float       s_cmd_temp = 0.0f;
// A change we didn't command, waiting to persist.
uint32_t    s_ext_since = 0;
std::string s_ext_mode;
float       s_ext_temp = 0.0f;
// Native AUTO waiting to be adopted.
uint32_t    s_adopt_since = 0;
float       s_adopt_temp = 0.0f;

uint32_t now_s() { return static_cast<uint32_t>(esp_timer_get_time() / 1000000); }

std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool same(float a, float b) { return std::fabs(a - b) < 0.01f; }

float step() {
    float st = s_hooks.step ? s_hooks.step() : 1.0f;
    return st > 0.0f ? st : 1.0f;
}

void save_locked() {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "on", s_on ? 1 : 0);
    nvs_set_i16(h, "lo", static_cast<int16_t>(std::lround(s_range.lo * 10.0f)));
    nvs_set_i16(h, "hi", static_cast<int16_t>(std::lround(s_range.hi * 10.0f)));
    nvs_set_u8(h, "dir", s_dir == Dir::Cool ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

// Best current room reading for a starting direction.
float room_now() {
    const park_ctl::Snapshot ps = park_ctl::snapshot();
    if (ps.sample.seq != 0) return ps.sample.room;
    return s_hooks.status ? s_hooks.status().roomTemperature : 0.0f;
}

// A unit command to send once the lock is released.
struct Out {
    bool        send = false;
    std::string mode;
    float       temp = 0.0f;
    bool        power_on = false;
    std::string event;
    events::Actor actor = events::Actor::System;
};

// Run the current direction: record the command so tick() doesn't read its
// echo as an outside change.
void command_locked(Out& o, bool power_on) {
    o.send = true;
    o.mode = autorange::dir_str(s_dir);
    o.temp = autorange::active(s_range, s_dir);
    o.power_on = power_on;
    s_cmd_mode = o.mode;
    s_cmd_temp = o.temp;
    s_cmd_until = now_s() + kCmdSettleS;
    s_ext_since = 0;
}

void finish(const Out& o) {
    if (o.send && s_hooks.apply) s_hooks.apply(o.mode, o.temp, o.power_on);
    if (!o.event.empty()) {
        ESP_LOGI(TAG, "%s", o.event.c_str());
        events::log(events::Cat::Mode, o.actor, o.event);
    }
    if ((o.send || !o.event.empty()) && s_hooks.on_change) s_hooks.on_change();
}

std::string fmt_c(float v) {
    char b[16];
    if (same(v, std::round(v))) snprintf(b, sizeof(b), "%.0f\xC2\xB0""C", v);
    else snprintf(b, sizeof(b), "%.1f\xC2\xB0""C", v);
    return b;
}

std::string range_str(const Range& r) {
    return "heat to " + fmt_c(r.lo) + ", cool to " + fmt_c(r.hi);
}

}  // namespace

void init(const Hooks& hooks) {
    std::lock_guard<std::mutex> lk(s_mtx);
    s_hooks = hooks;
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t on = 0, dir = 0;
    int16_t lo = 0, hi = 0;
    if (nvs_get_u8(h, "on", &on) == ESP_OK) s_on = on != 0;
    if (nvs_get_i16(h, "lo", &lo) == ESP_OK && nvs_get_i16(h, "hi", &hi) == ESP_OK)
        s_range = autorange::normalize({lo / 10.0f, hi / 10.0f}, 0.5f, autorange::Moved::Lo);
    if (nvs_get_u8(h, "dir", &dir) == ESP_OK) s_dir = dir ? Dir::Cool : Dir::Heat;
    nvs_close(h);
    ESP_LOGI(TAG, "managed Auto %s (heat to %.1f, cool to %.1f, %s)", s_on ? "on" : "off",
             s_range.lo, s_range.hi, autorange::dir_str(s_dir));
}

void tick() {
    if (!s_hooks.settings) return;
    // Dormant until the unit and the group config are known: a boot must not
    // read "no Tandem yet" as "Tandem turned off".
    if (!(s_hooks.connected && s_hooks.connected()) || !slice_ctl::configured()) return;
    const cn105::Settings s = s_hooks.settings();
    const std::string mode = upper(s.mode);
    const bool tandem = slice_ctl::enabled();
    const uint32_t t = now_s();
    const float st = step();
    Out o;
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        if (!s_on) {
            // Native AUTO under Tandem (IR remote, or Tandem just turned on):
            // adopt it as a range once its setpoint has settled.
            if (!tandem || mode != "AUTO") {
                s_adopt_since = 0;
            } else if (s_adopt_since == 0 || !same(s_adopt_temp, s.temperature)) {
                s_adopt_since = t;
                s_adopt_temp = s.temperature;
            } else if (t - s_adopt_since >= kAdoptS) {
                s_adopt_since = 0;
                s_range = autorange::from_single(s.temperature, st);
                s_dir = autorange::initial(room_now(), s_range);
                s_on = true;
                s_last_seq = park_ctl::snapshot().sample.seq;
                save_locked();
                command_locked(o, false);
                o.event = "Auto in Tandem: " + range_str(s_range);
            }
        } else if (!tandem) {
            // Tandem off: back to the unit's own Auto.
            s_on = false;
            save_locked();
            o.send = true;
            o.mode = "AUTO";
            o.temp = autorange::midpoint(s_range, st);
            o.event = "Tandem off: back to the unit's own Auto at " + fmt_c(o.temp);
        } else {
            if (s_cmd_until != 0 &&
                (t >= s_cmd_until || (mode == s_cmd_mode && same(s.temperature, s_cmd_temp))))
                s_cmd_until = 0;
            const std::string want_mode = autorange::dir_str(s_dir);
            const float want_temp = autorange::active(s_range, s_dir);
            const bool mismatch = mode != want_mode || !same(s.temperature, want_temp);
            if (s_cmd_until != 0 || !mismatch) {
                s_ext_since = 0;
            } else if (s_ext_since == 0 || mode != s_ext_mode || !same(s.temperature, s_ext_temp)) {
                s_ext_since = t;
                s_ext_mode = mode;
                s_ext_temp = s.temperature;
            } else if (t - s_ext_since >= kExternalS) {
                // Changed on the unit (IR remote, or a source we don't see).
                s_ext_since = 0;
                o.actor = events::Actor::Local;
                if (mode == "AUTO") {
                    s_range = autorange::from_single(s.temperature, st);
                    s_dir = autorange::initial(room_now(), s_range);
                    save_locked();
                    command_locked(o, false);
                    o.event = "Auto set on the unit: " + range_str(s_range);
                } else if (mode != want_mode) {
                    s_on = false;
                    save_locked();
                    o.event = "Auto off: unit set to " + mode;
                } else {
                    s_range = autorange::shift_active(s_range, s_dir, s.temperature, st);
                    save_locked();
                    if (!same(autorange::active(s_range, s_dir), s.temperature))
                        command_locked(o, false);
                    o.event = "Auto: " + range_str(s_range) + " (set on the unit)";
                }
            }

            // Heat or cool: act on each new trusted reading of the room.
            const park_ctl::Snapshot ps = park_ctl::snapshot();
            if (ps.sample.seq != s_last_seq) {
                s_last_seq = ps.sample.seq;
                const bool settled = !s_switched || t - s_switch_at >= kSwitchGuardS;
                if (s_on && !o.send && settled && s.power == "ON" && s_cmd_until == 0) {
                    const Dir nd = autorange::decide(s_dir, ps.sample.room, s_range,
                                                     park_ctl::config().delta_c);
                    if (nd != s_dir) {
                        s_dir = nd;
                        s_switch_at = t;
                        s_switched = true;
                        save_locked();
                        command_locked(o, false);
                        o.actor = events::Actor::System;
                        o.event = std::string("Auto: switched to ") +
                                  (nd == Dir::Heat ? "heating" : "cooling") + " (room " +
                                  fmt_c(ps.sample.room) + ")";
                    }
                }
            }
        }
    }
    finish(o);
}

bool active() {
    std::lock_guard<std::mutex> lk(s_mtx);
    return s_on;
}

View view() {
    std::lock_guard<std::mutex> lk(s_mtx);
    View v;
    v.on = s_on;
    v.range = s_range;
    v.dir = s_dir;
    return v;
}

cn105::Settings present(cn105::Settings s) {
    std::lock_guard<std::mutex> lk(s_mtx);
    if (s_on) s.mode = "AUTO";
    return s;
}

void enter() {
    const float room = room_now();
    Out o;
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        s_range = autorange::normalize(s_range, step(), autorange::Moved::Lo);
        if (!s_on) s_dir = autorange::initial(room, s_range);
        s_on = true;
        s_adopt_since = 0;
        s_last_seq = park_ctl::snapshot().sample.seq;
        save_locked();
        command_locked(o, true);
    }
    finish(o);
}

void leave() {
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        if (!s_on) return;
        s_on = false;
        s_cmd_until = 0;
        s_ext_since = 0;
        save_locked();
    }
    if (s_hooks.on_change) s_hooks.on_change();
}

void set_active(float t) {
    Out o;
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        if (!s_on) return;
        s_range = autorange::shift_active(s_range, s_dir, t, step());
        save_locked();
        command_locked(o, false);
    }
    finish(o);
}

bool set_range(float heat_to, float cool_to) {
    Out o;
    bool on;
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        // Keep the end that moved; if both did, keep heat-to.
        const autorange::Moved m = same(heat_to, s_range.lo) && !same(cool_to, s_range.hi)
                                       ? autorange::Moved::Hi
                                       : autorange::Moved::Lo;
        s_range = autorange::normalize({heat_to, cool_to}, step(), m);
        save_locked();
        on = s_on;
        if (on) command_locked(o, false);
    }
    finish(o);
    return on;
}

}  // namespace auto_ctl
