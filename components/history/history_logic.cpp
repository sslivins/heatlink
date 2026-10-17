/// @file history_logic.cpp
/// @brief Per-head history log — pure logic (see history_logic.h).

#include "history_logic.h"

#include <cmath>
#include <cstdio>

namespace history {

const char* const kFieldsJson =
    "[\"t\",\"room\",\"target\",\"outdoor\",\"sensor\",\"freq\",\"mode\",\"flags\",\"fan\"]";

uint8_t mode_code(const std::string& m) {
    if (m == "HEAT") return kModeHeat;
    if (m == "COOL") return kModeCool;
    if (m == "DRY")  return kModeDry;
    if (m == "FAN")  return kModeFan;
    if (m == "AUTO") return kModeAuto;
    return kModeNone;
}

uint8_t fan_code(const std::string& f) {
    if (f == "AUTO")  return 0;
    if (f == "QUIET") return 1;
    if (f.size() == 1 && f[0] >= '1' && f[0] <= '4') return (uint8_t)(f[0] - '1' + 2);
    return kNoFan;
}

int16_t to_deci(float c) {
    if (!std::isfinite(c)) return kNoTemp;
    long v = std::lround(c * 10.0f);
    if (v <= kNoTemp || v > INT16_MAX) return kNoTemp;
    return (int16_t)v;
}

uint32_t clamp_window(uint32_t w) {
    if (w == 0) return 86400;
    if (w < kMinWindowS) return kMinWindowS;
    if (w > kMaxWindowS) return kMaxWindowS;
    return w;
}

uint32_t bucket_for(uint32_t window_s) {
    uint32_t per = (window_s + kMaxPoints - 1) / kMaxPoints;
    uint32_t b = ((per + kIntervalS - 1) / kIntervalS) * kIntervalS;
    return b < kIntervalS ? kIntervalS : b;
}

void Bucketer::reset(uint32_t start) {
    open_ = true;
    start_ = start;
    n_ = 0;
    for (int i = 0; i < 4; i++) { sum_[i] = 0; cnt_[i] = 0; }
    for (auto& c : flag_cnt_) c = 0;
    freq_sum_ = 0;
    mode_ = kModeNone;
    fan_ = kNoFan;
}

void Bucketer::accumulate(const Record& r) {
    const int16_t temps[4] = {r.room, r.target, r.outdoor, r.sensor};
    for (int i = 0; i < 4; i++) {
        if (temps[i] == kNoTemp) continue;
        sum_[i] += temps[i];
        cnt_[i]++;
    }
    freq_sum_ += r.freq;
    for (int b = 0; b < 8; b++)
        if (r.flags & (1u << b)) flag_cnt_[b]++;
    mode_ = r.mode;
    fan_ = r.fan;
    n_++;
}

Record Bucketer::close() const {
    Record o;
    o.t = start_;
    int16_t* temps[4] = {&o.room, &o.target, &o.outdoor, &o.sensor};
    for (int i = 0; i < 4; i++) {
        if (!cnt_[i]) continue;
        // Round half away from zero so the mean of -0.5 steps isn't biased.
        int32_t s = sum_[i], c = (int32_t)cnt_[i];
        *temps[i] = (int16_t)(s >= 0 ? (s + c / 2) / c : -((-s + c / 2) / c));
    }
    o.freq = (uint8_t)((freq_sum_ + n_ / 2) / (n_ ? n_ : 1));
    o.mode = mode_;
    o.fan = fan_;
    for (int b = 0; b < 8; b++) {
        bool on = (1u << b) == kFlagDefrost ? flag_cnt_[b] > 0 : flag_cnt_[b] * 2 >= n_;
        if (on && flag_cnt_[b]) o.flags |= (uint8_t)(1u << b);
    }
    return o;
}

bool Bucketer::add(const Record& r, Record& out) {
    uint32_t start = r.t - r.t % bucket_s_;
    if (!open_) { reset(start); accumulate(r); return false; }
    if (start < start_) return false;
    if (start == start_) { accumulate(r); return false; }
    out = close();
    reset(start);
    accumulate(r);
    return true;
}

bool Bucketer::finish(Record& out) {
    if (!open_ || !n_) return false;
    out = close();
    open_ = false;
    return true;
}

int format_row(const Record& r, char* buf, size_t n) {
    char t[4][8];
    const int16_t temps[4] = {r.room, r.target, r.outdoor, r.sensor};
    for (int i = 0; i < 4; i++) {
        if (temps[i] == kNoTemp) snprintf(t[i], sizeof(t[i]), "null");
        else snprintf(t[i], sizeof(t[i]), "%d", (int)temps[i]);
    }
    char fan[6];
    if (r.fan == kNoFan) snprintf(fan, sizeof(fan), "null");
    else snprintf(fan, sizeof(fan), "%u", (unsigned)r.fan);
    return snprintf(buf, n, "[%lu,%s,%s,%s,%s,%u,%u,%u,%s]", (unsigned long)r.t,
                    t[0], t[1], t[2], t[3], (unsigned)r.freq, (unsigned)r.mode,
                    (unsigned)r.flags, fan);
}

}  // namespace history
