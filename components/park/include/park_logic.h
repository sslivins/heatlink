/// @file park_logic.h
/// @brief "Off at setpoint" (parking) state machine — pure, host-testable.
///
/// Mitsubishi wall heads keep the indoor fan running at thermo-off and can't be
/// told not to over CN105, so HeatLink emulates it: once a HEAT/COOL/DRY head
/// has reached its setpoint it is powered off ("parked") while the user's
/// desired state stays ON, then powered back on when the room drifts past the
/// setpoint or a periodic probe (a short run that moves real air past the
/// return sensor) finds the room needs conditioning.
///
/// This file has no ESP-IDF dependencies: the firmware feeds it unit snapshots
/// and a monotonic clock and executes the power Actions it returns. See
/// main/park_ctl.cpp for the glue and test/host/test_park_logic.cpp.

#pragma once

#include <cstdint>
#include <string>

namespace park {

/// Admin tunables. Always pass through sanitize() before use.
struct Config {
    bool     enabled          = false;
    float    delta_c          = 0.5f;  ///< restart threshold past setpoint, °C
    uint32_t min_off_s        = 300;   ///< minimum parked time before a drift wake
    bool     probe_enabled    = true;
    uint32_t probe_interval_s = 900;   ///< parked time between probes
    uint32_t probe_duration_s = 120;   ///< airflow time per probe
};

// Bounds enforced by sanitize().
constexpr float    kMinDeltaC         = 0.5f;
constexpr float    kMaxDeltaC         = 3.0f;
constexpr uint32_t kMinOffFloorS      = 300;   ///< outdoor-unit short-cycle floor
constexpr uint32_t kMaxOffS           = 3600;
constexpr uint32_t kMaxProbeIntervalS = 7200;
constexpr uint32_t kMinProbeDurationS = 60;
constexpr uint32_t kMaxProbeDurationS = 600;

// Fixed timings.
constexpr uint32_t kSatisfiedS     = 60;   ///< continuous thermo-off before parking
constexpr uint32_t kCmdSettleS     = 15;   ///< ignore unit/command mismatch after we command
constexpr uint32_t kExternalHoldS  = 8;    ///< mismatch must persist this long to count
constexpr uint32_t kProbeExtraMaxS = 600;  ///< cap on no-airflow time (preheat/defrost) per probe

/// Reason reported while a head is held off for the other mode's slice.
constexpr const char* kWaitingForSlice = "waiting_for_slice";

/// Clamp a config into the supported ranges (delta to 0.5 °C steps; probe
/// interval is never shorter than the minimum off time).
Config sanitize(const Config& c);

/// The outdoor-unit-wide subset of Config (enable, min off, probe on/interval/
/// duration) shared by every head in a group: "e,min_off,pe,pint,pdur".
/// delta_c stays per head. decode_shared() overlays the shared fields onto
/// @p inout (keeping its delta_c) and sanitizes; false (untouched) if malformed.
std::string encode_shared(const Config& c);
bool        decode_shared(const std::string& s, Config& inout);

enum class State { Off, Running, Parked, Probing };
const char* state_str(State s);  ///< "off" | "running" | "parked" | "probing"

enum class Action { None, PowerOn, PowerOff };

/// One snapshot of the indoor unit as reported over CN105.
struct Inputs {
    bool        connected = false;
    bool        phys_on   = false;   ///< unit reports power ON
    std::string mode;                ///< HEAT/COOL/DRY/FAN/AUTO
    float       setpoint  = 0.0f;
    float       room      = 0.0f;
    bool        operating = false;   ///< compressor running for this head
    std::string sub_mode;            ///< NORMAL/WARMUP/DEFROST/PREHEAT/STANDBY/OFF
    /// `room` comes from a fresh wireless room sensor (#142), not the head's
    /// return-air sensor: it is accurate while the fan is off, so no air checks.
    bool        room_external = false;
};

/// True for modes that park (HEAT, COOL, DRY — DRY is treated as COOL).
bool eligible_mode(const std::string& mode);

class Machine {
public:
    /// Apply new tunables. Returns the Action needed now (disabling while parked
    /// powers the unit back on).
    Action configure(const Config& c, uint32_t now_s);
    const Config& config() const { return cfg_; }

    /// Restore the persisted "managed" flag after a reboot. When true the head
    /// was parked/probing: desired power is ON even though the unit may be off.
    void restore(bool managed);

    /// The user (web UI / HA / group) set desired power.
    Action set_desired_power(bool on, uint32_t now_s);

    /// The user changed mode or setpoint. While parked this starts a probe so
    /// the new target is evaluated with real airflow; with a room sensor the new
    /// target is evaluated against its reading on the next tick instead.
    Action notify_intent_change(uint32_t now_s);

    /// Time-sliced heat/cool (#133): while held, a head whose desired power is
    /// ON is kept parked (reason "waiting_for_slice") with no drift wakes or
    /// probes, and a remote turn-on is re-parked. Releasing a held head powers it
    /// back on (normal parking then applies). Held/released transitions log no
    /// activity event (the slice coordinator logs slice changes).
    Action set_hold(bool hold, uint32_t now_s);
    bool   hold() const { return hold_; }

    /// Advance with a fresh unit snapshot (call ~1 Hz).
    Action tick(const Inputs& in, uint32_t now_s);

    State state() const { return state_; }
    bool  enabled() const { return cfg_.enabled; }
    /// Desired power is ON but the unit is held off.
    bool  parked() const { return cfg_.enabled && state_ == State::Parked; }
    /// Parked or probing: physical power may differ from the desired state, so
    /// this must survive a reboot.
    bool  managed() const {
        return cfg_.enabled && (state_ == State::Parked || state_ == State::Probing);
    }
    /// Parked because the setpoint was reached (not held for a slice): the
    /// slice scheduler's "this head is satisfied" signal.
    bool  parked_satisfied() const { return parked() && !hold_; }
    const char* reason() const { return reason_; }
    /// Seconds until the next probe while parked with probing on, else -1
    /// (also -1 while a room sensor makes probes unnecessary).
    int32_t next_probe_s(uint32_t now_s) const;

    /// A park/wake transition worth an activity-log entry since the last call,
    /// or nullptr. Clears it.
    const char* take_event();

private:
    Action enter_parked(uint32_t now_s, const char* why, const char* event);
    Action enter_probing(uint32_t now_s, const char* why, bool send);
    Action enter_running(uint32_t now_s, const char* event);
    Action command(bool on, uint32_t now_s);
    bool   satisfied(const Inputs& in) const;
    bool   needs_conditioning(const Inputs& in) const;

    Config   cfg_{};
    State    state_ = State::Off;
    bool     initialized_ = false;   ///< adopted the unit's state since enable/boot
    bool     restore_managed_ = false;
    bool     commanded_on_ = false;
    uint32_t cmd_at_ = 0;
    bool     mismatch_ = false;
    uint32_t mismatch_since_ = 0;
    bool     sat_ = false;
    uint32_t sat_since_ = 0;
    uint32_t parked_at_ = 0;
    uint32_t probe_ref_ = 0;         ///< probe interval measured from here
    uint32_t probe_start_ = 0;
    uint32_t probe_airflow_s_ = 0;
    bool     hold_ = false;
    bool     external_ = false;      ///< last tick's room came from a room sensor
    bool     reeval_ = false;        ///< target changed while parked with a sensor
    bool     have_tick_ = false;
    uint32_t last_tick_ = 0;
    const char* reason_ = "";
    const char* event_ = nullptr;
};

}  // namespace park
