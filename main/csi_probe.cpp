/// @file csi_probe.cpp
/// @brief Experimental passive Wi-Fi CSI occupancy probe. See csi_probe.h.
#include "csi_probe.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <ctime>
#include <mutex>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"

static const char* TAG = "csi";

namespace csi_probe {
namespace {

constexpr char     kNvsNs[]    = "csi";
constexpr int      kRawLen     = 128;          // LLTF: 64 subcarriers x (imag, real)
constexpr int64_t  kWindowUs   = 5 * 1000000;
constexpr size_t   kRing       = 720;          // 1 h of 5 s windows
constexpr float    kBaseAlpha  = 0.01f;        // ~8 min baseline time constant

struct Item {
    int8_t buf[kRawLen];
    int8_t rssi;
};

Hooks              s_hooks;
QueueHandle_t      s_q = nullptr;
TaskHandle_t       s_task = nullptr;
std::atomic<bool>  s_enabled{false};
std::atomic<bool>  s_csi_on{false};
std::atomic<uint32_t> s_rx{0};
std::atomic<uint32_t> s_dropped{0};
uint8_t            s_bssid[6] = {};            // written by the probe task only

std::mutex         s_ring_mtx;
Record*            s_ring = nullptr;
size_t             s_head = 0;                 // next write slot
size_t             s_count = 0;
uint32_t           s_seq = 0;

// Subcarrier index map into the 64-entry LLTF buffer: +2..+26 then -26..-1.
// Index 1 is skipped because the first 4 bytes can be invalid on this chip.
int sc_index(int k) { return k < 25 ? (2 + k) : (38 + (k - 25)); }

void csi_cb(void*, wifi_csi_info_t* info) {
    if (!info || !info->buf || info->len < kRawLen) return;
    if (info->rx_ctrl.sig_mode != 0) return;            // legacy 11g frames only
    if (memcmp(info->mac, s_bssid, 6) != 0) return;     // from our AP only
    Item it;
    memcpy(it.buf, info->buf, kRawLen);
    it.rssi = static_cast<int8_t>(info->rx_ctrl.rssi);
    if (xQueueSend(s_q, &it, 0) == pdTRUE) s_rx.fetch_add(1);
    else s_dropped.fetch_add(1);
}

bool refresh_bssid() {
    wifi_ap_record_t ap{};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return false;
    if (memcmp(ap.bssid, s_bssid, 6) == 0) return false;
    memcpy(s_bssid, ap.bssid, 6);
    return true;
}

void apply_csi(bool on) {
    if (on == s_csi_on.load()) return;
    if (on) {
        wifi_csi_config_t cfg{};
        cfg.lltf_en           = true;
        cfg.htltf_en          = false;
        cfg.stbc_htltf2_en    = false;
        cfg.ltf_merge_en      = false;
        cfg.channel_filter_en = false;   // keep subcarriers independent
        cfg.manu_scale        = false;
        cfg.shift             = 0;
        esp_err_t err = esp_wifi_set_csi_config(&cfg);
        if (err == ESP_OK) err = esp_wifi_set_csi_rx_cb(csi_cb, nullptr);
        if (err == ESP_OK) err = esp_wifi_set_csi(true);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "enable failed: %s", esp_err_to_name(err));
            return;
        }
        ESP_LOGI(TAG, "CSI capture on");
    } else {
        esp_wifi_set_csi(false);
        ESP_LOGI(TAG, "CSI capture off");
    }
    s_csi_on = on;
}

float pearson(const float* a, const float* b) {
    // Both vectors are normalised to mean 1.
    float sab = 0, saa = 0, sbb = 0;
    for (int k = 0; k < kSubcarriers; k++) {
        const float x = a[k] - 1.0f, y = b[k] - 1.0f;
        sab += x * y; saa += x * x; sbb += y * y;
    }
    if (saa <= 0 || sbb <= 0) return 1.0f;
    return sab / std::sqrt(saa * sbb);
}

uint16_t clamp16(float v) { return v < 0 ? 0 : v > 65535 ? 65535 : static_cast<uint16_t>(v + 0.5f); }

void push(const Record& r) {
    std::lock_guard<std::mutex> lk(s_ring_mtx);
    if (!s_ring) return;
    s_ring[s_head] = r;
    s_ring[s_head].seq = ++s_seq;
    s_head = (s_head + 1) % kRing;
    if (s_count < kRing) s_count++;
}

void probe_task(void*) {
    float prev[kSubcarriers], cur[kSubcarriers];
    float sum[kSubcarriers], sumsq[kSubcarriers];
    float base[kSubcarriers];
    bool  have_prev = false, have_base = false;
    float change_sum = 0; int change_n = 0;
    float rssi_sum = 0, rssi_sq = 0;
    int   n = 0;
    bool  bssid_changed = refresh_bssid();
    memset(sum, 0, sizeof(sum)); memset(sumsq, 0, sizeof(sumsq));
    int64_t window_end = esp_timer_get_time() + kWindowUs;

    while (true) {
        if (!s_enabled.load()) {
            apply_csi(false);
            Item drain;
            while (xQueueReceive(s_q, &drain, 0) == pdTRUE) {}
            vTaskDelay(pdMS_TO_TICKS(1000));
            window_end = esp_timer_get_time() + kWindowUs;
            n = 0; change_n = 0; change_sum = rssi_sum = rssi_sq = 0;
            memset(sum, 0, sizeof(sum)); memset(sumsq, 0, sizeof(sumsq));
            have_prev = false;
            continue;
        }
        apply_csi(true);

        const int64_t now = esp_timer_get_time();
        Item it;
        const TickType_t wait = now >= window_end ? 0 : pdMS_TO_TICKS((window_end - now) / 1000 + 1);
        if (xQueueReceive(s_q, &it, wait) == pdTRUE) {
            float m = 0;
            for (int k = 0; k < kSubcarriers; k++) {
                const int i = sc_index(k);
                const float im = it.buf[2 * i], re = it.buf[2 * i + 1];
                cur[k] = std::sqrt(re * re + im * im);
                m += cur[k];
            }
            m /= kSubcarriers;
            if (m >= 1.0f) {
                for (int k = 0; k < kSubcarriers; k++) {
                    cur[k] /= m;
                    sum[k] += cur[k];
                    sumsq[k] += cur[k] * cur[k];
                }
                if (have_prev) { change_sum += 1.0f - pearson(cur, prev); change_n++; }
                memcpy(prev, cur, sizeof(cur));
                have_prev = true;
                rssi_sum += it.rssi; rssi_sq += float(it.rssi) * it.rssi;
                n++;
            }
        }
        if (esp_timer_get_time() < window_end) continue;

        Record r{};
        r.n = static_cast<uint16_t>(n);
        r.bssid_changed = bssid_changed;
        if (n > 0) {
            const float rm = rssi_sum / n;
            r.rssi = static_cast<int8_t>(std::lround(rm));
            r.rssi_sd = static_cast<uint8_t>(std::fmin(255.0f, 10.0f * std::sqrt(std::fmax(0.0f, rssi_sq / n - rm * rm))));
            float mean[kSubcarriers], jit = 0;
            for (int k = 0; k < kSubcarriers; k++) {
                mean[k] = sum[k] / n;
                jit += std::sqrt(std::fmax(0.0f, sumsq[k] / n - mean[k] * mean[k]));
                const float q = mean[k] * 64.0f;
                r.spectrum[k] = static_cast<uint8_t>(q < 0 ? 0 : q > 255 ? 255 : q + 0.5f);
            }
            r.jitter = clamp16(1000.0f * jit / kSubcarriers);
            if (change_n > 0) r.change = clamp16(1e4f * change_sum / change_n);
            if (!have_base || bssid_changed) {
                memcpy(base, mean, sizeof(mean));
                have_base = true;
            } else {
                r.wander = clamp16(1e4f * (1.0f - pearson(mean, base)));
                for (int k = 0; k < kSubcarriers; k++) base[k] += kBaseAlpha * (mean[k] - base[k]);
            }
        }
        const time_t tnow = time(nullptr);
        r.t = tnow > 1600000000 ? static_cast<uint32_t>(tnow) : 0;
        if (s_hooks.unit) r.unit = s_hooks.unit();
        push(r);

        n = 0; change_n = 0; change_sum = rssi_sum = rssi_sq = 0;
        memset(sum, 0, sizeof(sum)); memset(sumsq, 0, sizeof(sumsq));
        window_end += kWindowUs;
        if (window_end < esp_timer_get_time()) window_end = esp_timer_get_time() + kWindowUs;
        bssid_changed = refresh_bssid();
        if (bssid_changed) have_prev = false;
    }
}

void ensure_task() {
    if (s_task) return;
    s_ring = static_cast<Record*>(heap_caps_calloc(kRing, sizeof(Record), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_ring) s_ring = static_cast<Record*>(calloc(kRing, sizeof(Record)));
    s_q = xQueueCreate(32, sizeof(Item));
    if (!s_ring || !s_q) {
        ESP_LOGE(TAG, "out of memory");
        return;
    }
    xTaskCreate(probe_task, "csi", 4096, nullptr, 2, &s_task);
}

}  // namespace

void init(const Hooks& hooks) {
    s_hooks = hooks;
    nvs_handle_t h;
    uint8_t en = 0;
    if (nvs_open(kNvsNs, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "en", &en);
        nvs_close(h);
    }
    if (en) {
        s_enabled = true;
        ensure_task();
    }
}

void set_enabled(bool on) {
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "en", on ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
    s_enabled = on;
    if (on) ensure_task();
    ESP_LOGI(TAG, "probe %s", on ? "enabled" : "disabled");
}

bool enabled() { return s_enabled.load(); }

Stats stats() {
    Stats s;
    s.enabled = s_enabled.load();
    memcpy(s.bssid, s_bssid, 6);
    s.rx = s_rx.load();
    s.dropped = s_dropped.load();
    std::lock_guard<std::mutex> lk(s_ring_mtx);
    s.last_seq = s_seq;
    return s;
}

std::vector<Record> records_since(uint32_t since, size_t limit) {
    std::vector<Record> out;
    std::lock_guard<std::mutex> lk(s_ring_mtx);
    if (!s_ring || s_count == 0) return out;
    const size_t start = (s_head + kRing - s_count) % kRing;
    for (size_t i = 0; i < s_count && out.size() < limit; i++) {
        const Record& r = s_ring[(start + i) % kRing];
        if (r.seq > since) out.push_back(r);
    }
    return out;
}

}  // namespace csi_probe
