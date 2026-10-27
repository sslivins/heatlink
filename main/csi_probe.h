/// @file csi_probe.h
/// @brief Experimental Wi-Fi CSI (channel state information) occupancy probe.
///
/// Passive only: it listens to the legacy-rate frames the connected AP already
/// sends (beacons ~10/s), so it adds no transmit traffic and no extra load on
/// the CN105 5 V rail. Every 5 s it reduces the packets to a few motion
/// features plus a compact mean amplitude spectrum, tags them with the indoor
/// unit's fan/vane/compressor state (the unit's own moving parts disturb the
/// channel too), and keeps the last hour in a RAM ring for offline analysis.
///
/// Off by default; enabled per head through POST /api/diag/csi (persisted).
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace csi_probe {

constexpr int kSubcarriers = 51;   // LLTF subcarriers 2..26 and -26..-1

struct UnitSnap {
    bool    connected  = false;
    bool    power      = false;
    bool    operating  = false;
    uint8_t mode       = 0;   // history::mode_code
    uint8_t fan        = 0;   // history::fan_code (setting)
    uint8_t stage      = 0;   // actual fan stage: 0 idle/unknown .. 5 high
    uint8_t vane       = 0;   // 0 auto, 1..5, 6 swing, 255 unknown
    uint8_t wide_vane  = 0;   // 0..6 (<< < | > >> <> swing), 255 unknown
    uint8_t freq       = 0;   // compressor Hz
};

struct Record {
    uint32_t seq       = 0;
    uint32_t t         = 0;   // unix s at window end (0 before SNTP)
    uint16_t n         = 0;   // packets in the window
    int8_t   rssi      = 0;   // mean RSSI, dBm
    uint8_t  rssi_sd   = 0;   // RSSI std-dev x10
    uint16_t change    = 0;   // mean (1 - corr) between consecutive packets x1e4
    uint16_t jitter    = 0;   // mean per-subcarrier std of normalised amp x1e3
    uint16_t wander    = 0;   // (1 - corr) of window mean vs slow baseline x1e4
    bool     bssid_changed = false;
    UnitSnap unit;
    uint8_t  spectrum[kSubcarriers] = {};  // window mean normalised amp x64
};

struct Hooks {
    std::function<UnitSnap()> unit;
};

/// Register hooks and start the probe if it was left enabled. Call after
/// Wi-Fi is up.
void init(const Hooks& hooks);

void set_enabled(bool on);   // persisted in NVS
bool enabled();

struct Stats {
    bool     enabled = false;
    uint8_t  bssid[6] = {};
    uint32_t rx      = 0;   // CSI packets accepted
    uint32_t dropped = 0;   // queue overflow
    uint32_t last_seq = 0;
};
Stats stats();

/// Records with seq > @p since, oldest first, at most @p limit.
std::vector<Record> records_since(uint32_t since, size_t limit);

}  // namespace csi_probe
