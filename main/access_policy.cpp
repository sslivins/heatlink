/// @file access_policy.cpp
/// @brief See access_policy.h.
#include "access_policy.h"

#include <cmath>
#include <mutex>

#include "nvs.h"

namespace access_policy {
namespace {

const char* kNs = "access";
constexpr float kMinC = 16.0f, kMaxC = 31.0f;

std::mutex g_mtx;
bool g_loaded = false;
Limits g_lim;
bool g_power_reset = true;

void load_locked() {
    if (g_loaded) return;
    g_loaded = true;
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t u8;
    int16_t i16;
    if (nvs_get_u8(h, "lim_en", &u8) == ESP_OK) g_lim.enabled = u8 != 0;
    if (nvs_get_i16(h, "lim_lo", &i16) == ESP_OK) g_lim.min_c = i16 / 10.0f;
    if (nvs_get_i16(h, "lim_hi", &i16) == ESP_OK) g_lim.max_c = i16 / 10.0f;
    if (nvs_get_u8(h, "pwr_rst", &u8) == ESP_OK) g_power_reset = u8 != 0;
    nvs_close(h);
}

bool valid_c(float c) {
    return c >= kMinC && c <= kMaxC && std::fabs(c * 2 - std::round(c * 2)) < 1e-3f;
}

}  // namespace

void init() {
    std::lock_guard<std::mutex> lk(g_mtx);
    load_locked();
}

Limits limits() {
    std::lock_guard<std::mutex> lk(g_mtx);
    load_locked();
    return g_lim;
}

bool set_limits(const Limits& l) {
    if (!valid_c(l.min_c) || !valid_c(l.max_c) || l.min_c > l.max_c) return false;
    std::lock_guard<std::mutex> lk(g_mtx);
    load_locked();
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_set_u8(h, "lim_en", l.enabled ? 1 : 0);
    nvs_set_i16(h, "lim_lo", (int16_t)std::lround(l.min_c * 10));
    nvs_set_i16(h, "lim_hi", (int16_t)std::lround(l.max_c * 10));
    bool ok = nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (ok) g_lim = l;
    return ok;
}

float clamp_user_setpoint(float c) {
    Limits l = limits();
    if (!l.enabled) return c;
    if (c < l.min_c) return l.min_c;
    if (c > l.max_c) return l.max_c;
    return c;
}

bool power_reset_enabled() {
    std::lock_guard<std::mutex> lk(g_mtx);
    load_locked();
    return g_power_reset;
}

void set_power_reset_enabled(bool on) {
    std::lock_guard<std::mutex> lk(g_mtx);
    load_locked();
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "pwr_rst", on ? 1 : 0);
    if (nvs_commit(h) == ESP_OK) g_power_reset = on;
    nvs_close(h);
}

}  // namespace access_policy
