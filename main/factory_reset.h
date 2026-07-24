/// @file factory_reset.h
/// @brief Full factory reset (wipe all persisted config) + a physical trigger.
///
/// A factory reset erases the entire default NVS partition — Wi-Fi credentials,
/// MQTT broker settings, group membership, the device name, and web/API
/// passwords — then reboots, so the controller comes back up in first-run
/// provisioning (SoftAP / OOBE) exactly like a freshly flashed unit.
///
/// Triggered from software only: POST /api/system/factory_reset (handled in
/// web_ui.cpp) calls perform() after flushing its HTTP response.
///
/// There is no physical trigger on the Stamp-S3Bat: the top button is wired to
/// the PY32 PMIC (I2C 0x6E), not an ESP GPIO, and the PMIC firmware owns its
/// click/hold gestures (single=On/Reset, double=Off, hold=download), so the ESP
/// can't repurpose it for a hold-to-reset without hijacking the power button.
#pragma once

namespace factory_reset {

/// Erase the whole default NVS partition and reboot. Does not return.
/// Safe to call from any task; used by the HTTP handler.
[[noreturn]] void perform();

}  // namespace factory_reset
