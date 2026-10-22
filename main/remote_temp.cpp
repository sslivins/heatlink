/// @file remote_temp.cpp
#include "remote_temp.h"

#include <algorithm>
#include <mutex>

#include "esp_log.h"
#include "esp_timer.h"

namespace remote_temp {
namespace {

const char* TAG = "remote_temp";

std::mutex s_mtx;
Hooks s_hooks;
esp_timer_handle_t s_timer = nullptr;

bool s_active = false;
float s_temp = 0;
std::string s_source, s_who;
int64_t s_updated_us = 0;
int64_t s_expires_us = 0;  // 0: no expiry

int64_t now_us() { return esp_timer_get_time(); }

// Caller holds s_mtx.
void reset_locked() {
    s_active = false;
    s_temp = 0;
    s_expires_us = 0;
}

void check(void*) {
    std::string msg, source;
    bool send_clear = false;
    {
        std::lock_guard<std::mutex> g(s_mtx);
        if (!s_active) return;
        source = s_source;
        if (s_hooks.sensor_active && s_hooks.sensor_active()) {
            // The room sensors took over; they also clear when they lose it.
            reset_locked();
            msg = "Remote temperature from " + s_who + " replaced by the room sensor";
        } else if (s_expires_us && now_us() >= s_expires_us) {
            reset_locked();
            send_clear = true;
            msg = "Remote temperature from " + s_who + " expired; using the unit's sensor";
        } else {
            return;
        }
    }
    ESP_LOGW(TAG, "%s", msg.c_str());
    if (send_clear && s_hooks.set_remote) s_hooks.set_remote(0.0f);
    if (s_hooks.on_event) s_hooks.on_event(msg, source, "");
}

}  // namespace

void init(const Hooks& hooks) {
    s_hooks = hooks;
    if (s_timer) return;
    esp_timer_create_args_t a = {};
    a.callback = &check;
    a.name = "rtemp";
    if (esp_timer_create(&a, &s_timer) == ESP_OK)
        esp_timer_start_periodic(s_timer, 5 * 1000000LL);
}

Res set(float celsius, uint32_t ttl_s, const std::string& source, const std::string& who) {
    if (!(celsius >= kMinC && celsius <= kMaxC)) return Res::OutOfRange;
    if (s_hooks.sensor_active && s_hooks.sensor_active()) return Res::SensorOwns;
    bool started;
    {
        std::lock_guard<std::mutex> g(s_mtx);
        started = !s_active;
        s_active = true;
        s_temp = celsius;
        s_source = source;
        s_who = who.empty() ? source : who;
        s_updated_us = now_us();
        s_expires_us = ttl_s == 0
                           ? 0
                           : s_updated_us +
                                 (int64_t)std::clamp(ttl_s, kMinTtlS, kMaxTtlS) * 1000000LL;
    }
    if (s_hooks.set_remote) s_hooks.set_remote(celsius);
    if (started && s_hooks.on_event)
        s_hooks.on_event("Remote temperature feed started", source, who);
    return Res::Ok;
}

void clear(const std::string& who) {
    std::string source;
    {
        std::lock_guard<std::mutex> g(s_mtx);
        if (!s_active) return;
        source = s_source;
        reset_locked();
    }
    if (s_hooks.set_remote) s_hooks.set_remote(0.0f);
    if (s_hooks.on_event)
        s_hooks.on_event("Remote temperature cleared; using the unit's sensor", source, who);
}

Snapshot snapshot() {
    std::lock_guard<std::mutex> g(s_mtx);
    Snapshot s;
    if (!s_active) return s;
    const int64_t now = now_us();
    s.active = true;
    s.temp = s_temp;
    s.source = s_source;
    s.who = s_who;
    s.age_s = (uint32_t)((now - s_updated_us) / 1000000);
    s.expires_in_s =
        s_expires_us ? (int32_t)std::max<int64_t>(0, (s_expires_us - now) / 1000000) : -1;
    return s;
}

}  // namespace remote_temp
