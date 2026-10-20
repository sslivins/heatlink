/// @file devlog.h
/// @brief Capture of the firmware's ESP_LOG output in a RAM ring, served at the
/// admin-only GET /api/logs to diagnose sporadic problems (power sags,
/// brownouts, Wi-Fi drops) after the fact without a serial cable.
///
/// The ring lives in no-init RAM, so it carries on across software, panic,
/// watchdog and (normally) brownout restarts — the lines leading up to a reset
/// are still there afterwards, followed by a "restart" marker. It is wiped on a
/// power-on reset or if its header fails validation.
#pragma once

#include <string>

namespace devlog {

/// Install the log hook. Call first thing in app_main.
void init();

/// The captured log, oldest first.
std::string snapshot();

/// Discard the captured log.
void clear();

}  // namespace devlog
