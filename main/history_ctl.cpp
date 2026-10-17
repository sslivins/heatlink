/// @file history_ctl.cpp
/// @brief Per-head history log — sampling task and SPIFFS storage.

#include "history_ctl.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <mutex>
#include <sys/stat.h>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

using history::Record;

namespace {

const char* TAG = "history";

constexpr const char* kDir = "/spiffs";
// A wall clock before this (2023-11-14) hasn't been set by SNTP yet.
constexpr time_t   kClockValidAfter = 1700000000;
constexpr int      kPending = 20;                 // 10 min of samples
constexpr size_t   kMaxDayBytes = 64 * 1024;      // > one day (2880 × 16 B)

std::mutex      g_mtx;
Record          g_pending[kPending];
int             g_npending = 0;
history_ctl::Hooks g_hooks;
uint32_t        g_earliest = 0;      // oldest record on flash, 0 = none
uint32_t        g_scanned_day = 0;   // UTC day of the last prune

void day_path(uint32_t day, char* buf, size_t n) {
    snprintf(buf, n, "%s/h%lu.bin", kDir, (unsigned long)day);
}

// Parse "h<day>.bin" (SPIFFS may or may not hand back a leading '/').
bool parse_day(const char* name, uint32_t& day) {
    if (*name == '/') name++;
    unsigned long d = 0;
    char ext[8] = {0};
    if (sscanf(name, "h%lu.%7s", &d, ext) != 2 || strcmp(ext, "bin") != 0) return false;
    day = (uint32_t)d;
    return true;
}

// Append the RAM buffer to the per-day files. Caller holds g_mtx.
void flush_locked() {
    int i = 0;
    while (i < g_npending) {
        uint32_t day = g_pending[i].t / 86400;
        int j = i;
        while (j < g_npending && g_pending[j].t / 86400 == day) j++;
        char path[32];
        day_path(day, path, sizeof(path));
        FILE* f = fopen(path, "ab");
        if (!f) {
            ESP_LOGW(TAG, "open %s failed; dropping %d sample(s)", path, j - i);
        } else {
            size_t w = fwrite(&g_pending[i], sizeof(Record), (size_t)(j - i), f);
            fclose(f);
            if (w != (size_t)(j - i)) ESP_LOGW(TAG, "short write to %s (%u/%d)", path, (unsigned)w, j - i);
            if (!g_earliest || g_pending[i].t < g_earliest) g_earliest = g_pending[i].t;
        }
        i = j;
    }
    g_npending = 0;
}

// Find the oldest day file and, when @p today is set, delete files outside the
// retention window (older than kRetainDays full days, or dated in the future
// after a bad clock). Caller holds g_mtx.
void scan_locked(uint32_t today) {
    DIR* d = opendir(kDir);
    if (!d) return;
    std::vector<uint32_t> doomed;
    uint32_t oldest = UINT32_MAX;
    while (dirent* e = readdir(d)) {
        uint32_t day;
        if (!parse_day(e->d_name, day)) continue;
        if (today && (day + history::kRetainDays < today || day > today + 1)) {
            doomed.push_back(day);
            continue;
        }
        if (day < oldest) oldest = day;
    }
    closedir(d);
    char path[32];
    for (uint32_t day : doomed) {
        day_path(day, path, sizeof(path));
        remove(path);
        ESP_LOGI(TAG, "pruned %s", path);
    }
    g_earliest = 0;
    if (oldest == UINT32_MAX) return;
    day_path(oldest, path, sizeof(path));
    if (FILE* f = fopen(path, "rb")) {
        Record r;
        if (fread(&r, sizeof(r), 1, f) == 1) g_earliest = r.t;
        fclose(f);
    }
}

void on_shutdown() {
    // Best effort: an OTA or reboot shouldn't drop the last 10 minutes.
    if (g_mtx.try_lock()) {
        flush_locked();
        g_mtx.unlock();
    }
}

void task(void*) {
    uint32_t last_slot = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        time_t now = time(nullptr);
        if (now < kClockValidAfter) continue;
        uint32_t slot = (uint32_t)now - (uint32_t)now % history::kIntervalS;
        if (slot == last_slot) continue;
        last_slot = slot;

        Record r;
        if (g_hooks.sample) g_hooks.sample(r);
        r.t = slot;

        uint32_t today = slot / 86400;
        std::lock_guard<std::mutex> lock(g_mtx);
        g_pending[g_npending++] = r;
        if (g_npending == kPending) flush_locked();
        if (today != g_scanned_day) {
            flush_locked();
            scan_locked(today);
            g_scanned_day = today;
        }
    }
}

}  // namespace

namespace history_ctl {

void init(const Hooks& hooks) {
    g_hooks = hooks;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        scan_locked(0);
    }
    esp_register_shutdown_handler(on_shutdown);
    xTaskCreate(task, "history", 4096, nullptr, 2, nullptr);
    ESP_LOGI(TAG, "ready, earliest=%lu", (unsigned long)g_earliest);
}

uint32_t earliest() {
    std::lock_guard<std::mutex> lock(g_mtx);
    if (g_earliest) return g_earliest;
    return g_npending ? g_pending[0].t : 0;
}

void flush() {
    std::lock_guard<std::mutex> lock(g_mtx);
    flush_locked();
}

void query(uint32_t start, uint32_t end, const std::function<void(const Record&)>& fn) {
    if (end <= start) return;
    // Snapshot the RAM buffer first: if it's flushed while the files are being
    // read, its records show up in both and the copy is skipped below.
    Record pending[kPending];
    int npending;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        npending = g_npending;
        memcpy(pending, g_pending, sizeof(Record) * (size_t)npending);
    }

    uint32_t last = 0;
    for (uint32_t day = start / 86400; day <= (end - 1) / 86400; day++) {
        uint8_t* buf = nullptr;
        size_t n = 0;
        {
            std::lock_guard<std::mutex> lock(g_mtx);
            char path[32];
            day_path(day, path, sizeof(path));
            struct stat st;
            if (stat(path, &st) != 0 || st.st_size <= 0) continue;
            size_t size = (size_t)st.st_size;
            if (size > kMaxDayBytes) size = kMaxDayBytes;
            size -= size % sizeof(Record);
            buf = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!buf) buf = (uint8_t*)malloc(size);
            if (!buf) continue;
            if (FILE* f = fopen(path, "rb")) {
                n = fread(buf, sizeof(Record), size / sizeof(Record), f);
                fclose(f);
            }
        }
        for (size_t i = 0; i < n; i++) {
            Record r;
            memcpy(&r, buf + i * sizeof(Record), sizeof(Record));
            if (r.t < start || r.t >= end) continue;
            fn(r);
            if (r.t > last) last = r.t;
        }
        free(buf);
    }
    for (int i = 0; i < npending; i++) {
        const Record& r = pending[i];
        if (r.t <= last || r.t < start || r.t >= end) continue;
        fn(r);
    }
}

}  // namespace history_ctl
