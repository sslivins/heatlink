/// @file park_logic.cpp
/// @brief Off-at-setpoint state machine (see park_logic.h). No ESP-IDF deps.

#include "park_logic.h"

#include <cmath>

namespace park {

namespace {

constexpr float kEps = 0.01f;

bool is_heat(const std::string& m) { return m == "HEAT"; }
bool is_cool(const std::string& m) { return m == "COOL" || m == "DRY"; }

// The indoor fan is held off (or the unit is busy) in these sub-modes, so a
// thermo-off there doesn't mean "satisfied" and probe time there moves no air.
bool fan_held(const std::string& sub) {
    return sub == "PREHEAT" || sub == "DEFROST" || sub == "WARMUP";
}

uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

}  // namespace

Config sanitize(const Config& c) {
    Config o = c;
    float d = std::isfinite(c.delta_c) ? c.delta_c : kMinDeltaC;
    d = std::round(d * 2.0f) / 2.0f;
    if (d < kMinDeltaC) d = kMinDeltaC;
    if (d > kMaxDeltaC) d = kMaxDeltaC;
    o.delta_c = d;
    o.min_off_s = clamp_u32(c.min_off_s, kMinOffFloorS, kMaxOffS);
    o.probe_interval_s = clamp_u32(c.probe_interval_s, o.min_off_s, kMaxProbeIntervalS);
    o.probe_duration_s = clamp_u32(c.probe_duration_s, kMinProbeDurationS, kMaxProbeDurationS);
    return o;
}

const char* state_str(State s) {
    switch (s) {
        case State::Running: return "running";
        case State::Parked:  return "parked";
        case State::Probing: return "probing";
        default:             return "off";
    }
}

bool eligible_mode(const std::string& mode) { return is_heat(mode) || is_cool(mode); }

bool Machine::satisfied(const Inputs& in) const {
    if (in.room <= 0.0f) return false;  // no reading yet
    if (is_heat(in.mode)) return in.room >= in.setpoint - kEps;
    if (is_cool(in.mode)) return in.room <= in.setpoint + kEps;
    return false;
}

bool Machine::needs_conditioning(const Inputs& in) const {
    if (in.room <= 0.0f) return false;
    if (is_heat(in.mode)) return in.room <= in.setpoint - cfg_.delta_c + kEps;
    if (is_cool(in.mode)) return in.room >= in.setpoint + cfg_.delta_c - kEps;
    return false;
}

Action Machine::command(bool on, uint32_t now_s) {
    commanded_on_ = on;
    cmd_at_ = now_s;
    mismatch_ = false;
    return on ? Action::PowerOn : Action::PowerOff;
}

Action Machine::enter_parked(uint32_t now_s, const char* why, const char* event) {
    state_ = State::Parked;
    parked_at_ = now_s;
    probe_ref_ = now_s;
    sat_ = false;
    reason_ = why;
    if (event) event_ = event;
    return command(false, now_s);
}

Action Machine::enter_probing(uint32_t now_s, const char* why, bool send) {
    state_ = State::Probing;
    probe_start_ = now_s;
    probe_airflow_s_ = 0;
    sat_ = false;
    reason_ = why;
    if (!send) return Action::None;
    return command(true, now_s);
}

Action Machine::enter_running(uint32_t now_s, const char* event) {
    state_ = State::Running;
    sat_ = false;
    reason_ = "";
    if (event) event_ = event;
    return commanded_on_ ? Action::None : command(true, now_s);
}

Action Machine::configure(const Config& c, uint32_t now_s) {
    const bool was = cfg_.enabled;
    const bool was_managed = managed();
    cfg_ = sanitize(c);
    if (was && !cfg_.enabled) {
        initialized_ = false;
        if (was_managed) {
            state_ = State::Running;
            reason_ = "";
            event_ = "Off at setpoint disabled \xE2\x80\x94 resumed";
            return command(true, now_s);
        }
    } else if (!was && cfg_.enabled) {
        initialized_ = false;  // adopt the unit's current state on the next tick
    }
    return Action::None;
}

void Machine::restore(bool managed) {
    restore_managed_ = managed;
    initialized_ = false;
}

Action Machine::set_desired_power(bool on, uint32_t now_s) {
    if (!cfg_.enabled || !initialized_) {
        state_ = on ? State::Running : State::Off;
        restore_managed_ = false;
        return command(on, now_s);
    }
    if (!on) {
        state_ = State::Off;
        sat_ = false;
        reason_ = "";
        return command(false, now_s);
    }
    if (state_ == State::Parked) return Action::None;  // already wants ON
    if (state_ == State::Off) state_ = State::Running;
    return command(true, now_s);
}

Action Machine::notify_intent_change(uint32_t now_s) {
    if (!cfg_.enabled || state_ != State::Parked) return Action::None;
    return enter_probing(now_s, "settings changed", true);
}

int32_t Machine::next_probe_s(uint32_t now_s) const {
    if (!parked() || !cfg_.probe_enabled) return -1;
    uint32_t since = now_s - probe_ref_;
    return since >= cfg_.probe_interval_s ? 0
                                          : static_cast<int32_t>(cfg_.probe_interval_s - since);
}

const char* Machine::take_event() {
    const char* e = event_;
    event_ = nullptr;
    return e;
}

Action Machine::tick(const Inputs& in, uint32_t now_s) {
    uint32_t dt = have_tick_ ? now_s - last_tick_ : 0;
    if (dt > 5) dt = 5;  // a stalled task must not fast-forward a probe
    have_tick_ = true;
    last_tick_ = now_s;

    if (!cfg_.enabled) {
        // A reboot can restore a parked flag after the feature was switched
        // off; the desired state was ON, so power the unit back on.
        if (restore_managed_ && in.connected) {
            restore_managed_ = false;
            state_ = State::Running;
            if (!in.phys_on) return command(true, now_s);
        }
        return Action::None;
    }
    if (!in.connected) {
        sat_ = false;
        mismatch_ = false;
        return Action::None;
    }

    if (!initialized_) {
        initialized_ = true;
        commanded_on_ = in.phys_on;
        cmd_at_ = now_s;
        mismatch_ = false;
        sat_ = false;
        if (restore_managed_) {
            restore_managed_ = false;
            if (in.phys_on) return enter_probing(now_s, "resumed after restart", false);
            state_ = State::Parked;
            parked_at_ = now_s;
            probe_ref_ = now_s;
            reason_ = "setpoint reached";
            return Action::None;
        }
        state_ = in.phys_on ? State::Running : State::Off;
        reason_ = "";
        return Action::None;
    }

    // External change (IR remote): the unit's power disagrees with what we last
    // commanded, well after our command, for long enough that it isn't the
    // driver briefly reverting a remote press inside its own grace window.
    if (in.phys_on != commanded_on_ && now_s - cmd_at_ >= kCmdSettleS) {
        if (!mismatch_) {
            mismatch_ = true;
            mismatch_since_ = now_s;
        }
        if (now_s - mismatch_since_ >= kExternalHoldS) {
            mismatch_ = false;
            commanded_on_ = in.phys_on;
            cmd_at_ = now_s;
            if (in.phys_on) {
                // A remote press while parked re-evaluates via a probe; while
                // off it is a plain turn-on.
                if (state_ == State::Parked) return enter_probing(now_s, "remote", false);
                state_ = State::Running;
                sat_ = false;
                reason_ = "";
            } else {
                state_ = State::Off;
                sat_ = false;
                reason_ = "";
            }
            return Action::None;
        }
    } else {
        mismatch_ = false;
    }

    switch (state_) {
        case State::Off:
            return Action::None;

        case State::Running: {
            if (!in.phys_on || !commanded_on_) return Action::None;
            const bool idle = eligible_mode(in.mode) && !in.operating &&
                              !fan_held(in.sub_mode) && in.sub_mode != "STANDBY" &&
                              satisfied(in);
            if (!idle) {
                sat_ = false;
                return Action::None;
            }
            if (!sat_) {
                sat_ = true;
                sat_since_ = now_s;
            }
            if (now_s - sat_since_ >= kSatisfiedS)
                return enter_parked(now_s, "setpoint reached", "Parked at setpoint");
            return Action::None;
        }

        case State::Parked: {
            if (!eligible_mode(in.mode)) return enter_running(now_s, nullptr);
            if (now_s - parked_at_ >= cfg_.min_off_s && needs_conditioning(in))
                return enter_running(now_s, "Resumed \xE2\x80\x94 room drifted from setpoint");
            if (cfg_.probe_enabled && now_s - probe_ref_ >= cfg_.probe_interval_s)
                return enter_probing(now_s, "scheduled probe", true);
            return Action::None;
        }

        case State::Probing: {
            if (!eligible_mode(in.mode)) return enter_running(now_s, nullptr);
            if (in.phys_on && !fan_held(in.sub_mode)) probe_airflow_s_ += dt;
            const bool done = probe_airflow_s_ >= cfg_.probe_duration_s ||
                              now_s - probe_start_ >= cfg_.probe_duration_s + kProbeExtraMaxS;
            if (!done) return Action::None;
            if (needs_conditioning(in))
                return enter_running(now_s, "Resumed \xE2\x80\x94 probe found room needs conditioning");
            return enter_parked(now_s, "setpoint reached", nullptr);
        }
    }
    return Action::None;
}

}  // namespace park
