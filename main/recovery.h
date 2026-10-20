/// @file recovery.h
/// @brief Admin-password reset procedure (power-cut sequence or HA button).
///
/// Cutting power to the head 5 times, with less than ~30 s between cuts,
/// clears the admin password and opens a 15-minute window in which a new one
/// can be set from the web UI (see auth_mgr_recover). Two detection paths so it
/// works with or without the backup LiPo:
///  - no battery: each power cut reboots the head, so a power-on/brownout reset
///    at boot counts as a cut;
///  - battery: the head rides through the cut, so pmic_task reports each
///    input-voltage drop-and-restore via note_power_restored().
#pragma once

namespace recovery {

/// Count this boot toward the sequence. Call once at boot after nvs_flash_init
/// and events::init.
void init();

/// Battery path: external input dropped out and came back while running.
void note_power_restored();

/// Run the reset procedure now. @p from_ha marks a Home Assistant trigger.
void trigger(bool from_ha);

}  // namespace recovery
