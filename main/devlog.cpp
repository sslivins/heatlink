/// @file devlog.cpp
/// @brief ESP_LOG capture ring — implementation.

#include "devlog.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace {

constexpr uint32_t kMagic = 0x474F4C44;  // "DLOG"
constexpr uint32_t kSize  = 16 * 1024;
// A signed epoch past this means SNTP has set the clock (2023-11-14).
constexpr time_t kClockValidAfter = 1700000000;

struct Ring {
    uint32_t magic;
    uint32_t head;   // next write offset
    uint32_t len;    // bytes held (<= kSize)
    uint32_t check;  // ties the header fields together so stale RAM fails validation
    char     buf[kSize];
};

__NOINIT_ATTR Ring s_ring;

SemaphoreHandle_t s_mtx = nullptr;
vprintf_like_t    s_prev = nullptr;
char              s_line[256];   // formatting scratch, guarded by s_mtx
bool              s_bol = true;  // next byte starts a line

uint32_t check_of(uint32_t head, uint32_t len) { return kMagic ^ (head * 2654435761u) ^ len; }

bool ring_valid() {
    return s_ring.magic == kMagic && s_ring.head < kSize && s_ring.len <= kSize &&
           s_ring.check == check_of(s_ring.head, s_ring.len);
}

void ring_reset() {
    s_ring.magic = kMagic;
    s_ring.head  = 0;
    s_ring.len   = 0;
    s_ring.check = check_of(0, 0);
}

void ring_put(const char* p, size_t n) {
    while (n > 0) {
        size_t chunk = kSize - s_ring.head;
        if (chunk > n) chunk = n;
        memcpy(s_ring.buf + s_ring.head, p, chunk);
        s_ring.head = (s_ring.head + chunk) % kSize;
        s_ring.len  = (s_ring.len + chunk > kSize) ? kSize : s_ring.len + chunk;
        p += chunk;
        n -= chunk;
    }
    s_ring.check = check_of(s_ring.head, s_ring.len);
}

// Append text, prefixing each new line with the UTC wall-clock time once SNTP
// has synced (ESP_LOG's own "(ms)" stamp is uptime only).
void put_text(const char* p, size_t n) {
    while (n > 0) {
        if (s_bol) {
            time_t now = time(nullptr);
            if (now > kClockValidAfter) {
                struct tm t;
                gmtime_r(&now, &t);
                char ts[24];
                size_t k = strftime(ts, sizeof(ts), "%m-%d %H:%M:%SZ ", &t);
                ring_put(ts, k);
            }
            s_bol = false;
        }
        const char* nl = (const char*)memchr(p, '\n', n);
        size_t take = nl ? (size_t)(nl - p) + 1 : n;
        ring_put(p, take);
        if (nl) s_bol = true;
        p += take;
        n -= take;
    }
}

int hook(const char* fmt, va_list ap) {
    va_list cp;
    va_copy(cp, ap);
    int r = s_prev ? s_prev(fmt, ap) : vprintf(fmt, ap);
    // Never block or format from an ISR or with the scheduler suspended; a line
    // that can't take the lock promptly is dropped from the capture (the UART
    // still gets it).
    if (s_mtx && !xPortInIsrContext() &&
        xTaskGetSchedulerState() == taskSCHEDULER_RUNNING &&
        xSemaphoreTake(s_mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
        int n = vsnprintf(s_line, sizeof(s_line), fmt, cp);
        if (n > 0) {
            size_t m = (size_t)n;
            if (m >= sizeof(s_line)) {  // truncated: keep the line break
                m = sizeof(s_line) - 1;
                s_line[m - 1] = '\n';
            }
            put_text(s_line, m);
        }
        xSemaphoreGive(s_mtx);
    }
    va_end(cp);
    return r;
}

const char* reason_str(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "interrupt watchdog";
        case ESP_RST_TASK_WDT:  return "task watchdog";
        case ESP_RST_WDT:       return "watchdog";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_EXT:       return "reset pin";
        case ESP_RST_DEEPSLEEP: return "deep sleep";
        default:                return "other";
    }
}

}  // namespace

namespace devlog {

void init() {
    s_mtx = xSemaphoreCreateMutex();
    esp_reset_reason_t why = esp_reset_reason();
    if (!ring_valid() || why == ESP_RST_POWERON || why == ESP_RST_UNKNOWN) {
        ring_reset();
    } else {
        char m[64];
        int n = snprintf(m, sizeof(m), "\n=== restart (%s) ===\n", reason_str(why));
        s_bol = true;
        ring_put(m, (size_t)n);
    }
    s_prev = esp_log_set_vprintf(hook);
}

std::string snapshot() {
    std::string out;
    if (!s_mtx) return out;
    out.reserve(kSize);  // allocate outside the lock
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    uint32_t start = (s_ring.head + kSize - s_ring.len) % kSize;
    uint32_t first = (start + s_ring.len > kSize) ? kSize - start : s_ring.len;
    out.append(s_ring.buf + start, first);
    out.append(s_ring.buf, s_ring.len - first);
    bool wrapped = s_ring.len == kSize;
    xSemaphoreGive(s_mtx);
    // A full ring starts mid-line: drop the partial first line.
    if (wrapped) {
        size_t nl = out.find('\n');
        if (nl != std::string::npos) out.erase(0, nl + 1);
    }
    return out;
}

void clear() {
    if (!s_mtx) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    ring_reset();
    s_bol = true;
    xSemaphoreGive(s_mtx);
}

}  // namespace devlog
