/// @file history_ctl.h
/// @brief Per-head history log: samples the unit every 30 s, keeps 14 days on
/// the SPIFFS `storage` partition (one file per UTC day of 16-byte records),
/// and serves time windows for the web UI's History chart.
///
/// Samples are buffered in RAM and appended to flash every 10 minutes (and on
/// a clean restart, e.g. OTA), so a power cut loses at most the last 10
/// minutes. Nothing is recorded until the wall clock has been set by SNTP.
#pragma once

#include <cstdint>
#include <functional>

#include "history_logic.h"

namespace history_ctl {

struct Hooks {
    /// Fill everything in @p r except @c t. Called from the history task.
    std::function<void(history::Record& r)> sample;
};

/// Start sampling. Call after events::init() (which mounts SPIFFS).
void init(const Hooks& hooks);

/// Oldest stored sample time (unix s), or 0 when nothing is recorded yet.
uint32_t earliest();

/// Visit the records with start <= t < end in time order (flash first, then
/// the unflushed RAM buffer). Runs on the caller's task; @p fn may block (it
/// is not called with the store's lock held).
void query(uint32_t start, uint32_t end,
           const std::function<void(const history::Record&)>& fn);

/// Write the RAM buffer to flash now.
void flush();

}  // namespace history_ctl
