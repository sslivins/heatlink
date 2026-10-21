/// @file slice_logic.h
/// @brief Time-sliced heat/cool for heads sharing one outdoor unit (#133) —
/// pure, host-testable.
///
/// In shoulder seasons some rooms want heat while others want cool, but one
/// outdoor unit can only serve one direction at a time. With time-slicing on,
/// the group alternates the outdoor unit between HEAT and COOL slices: heads
/// that want the current slice's mode run (parking at setpoint, #132), all
/// others are held parked until their slice comes up.
///
/// A turn is reconsidered after the max slice (or earlier, once the turn's
/// rooms are all satisfied and the min slice has passed). It is handed over
/// only if a waiting room needs it (Member::need: a room sensor, or a short
/// FAN-mode air check by the waiting head); otherwise the turn is extended by
/// another max slice.
///
/// Every head runs a Node. The coordinator (lowest reachable uid) runs the
/// scheduler and publishes its State in the signed group snapshot; the other
/// heads accept the coordinator's State, ordered by (term, seq) so a stale
/// ex-coordinator can't issue commands. A head that loses the coordinator for
/// kStaleS seconds falls back to running its own desired mode.
///
/// No ESP-IDF dependencies: see main/slice_ctl.cpp for the glue and
/// test/host/test_slice_logic.cpp for the tests.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace slice {

/// Version of the time-slice snapshot fields; every member must advertise it
/// before the mode can be enabled.
constexpr int kSliceVersion = 1;

enum class Mode : uint8_t { None, Heat, Cool };
const char* mode_str(Mode m);               ///< "none" | "heat" | "cool"
Mode        parse_mode(const std::string& s);
Mode        other(Mode m);                  ///< Heat<->Cool, None->None

/// A head's desired direction: HEAT -> Heat, COOL/DRY -> Cool, anything else
/// (OFF, FAN, AUTO, unknown) -> None (not scheduled).
Mode desired_of(bool power_on, const std::string& mode);

enum class Phase : uint8_t { Idle, Run, Changeover };
const char* phase_str(Phase p);             ///< "idle" | "run" | "changeover"
Phase       parse_phase(const std::string& s);

/// Group-wide admin settings (replicated to every head).
struct Config {
    bool     enabled     = false;
    uint32_t min_slice_s = 600;
    uint32_t max_slice_s = 1800;
    uint32_t settle_s    = 180;
};

constexpr uint32_t kMinSliceFloorS = 300;   ///< outdoor-unit short-cycle floor
constexpr uint32_t kMinSliceCapS   = 3600;
constexpr uint32_t kMaxSliceCapS   = 7200;
/// Matches the compressor's restart guard, in case the outdoor unit lacks one.
constexpr uint32_t kMinSettleS     = 180;
constexpr uint32_t kMaxSettleS     = 600;

/// A follower drops the coordinator's state after this long without a fresh
/// update.
constexpr uint32_t kStaleS  = 45;
/// After enable/boot a head waits this long (>= 2 poll rounds) before acting
/// as coordinator, so it first learns the group's current slice from peers.
constexpr uint32_t kWarmupS = 30;
/// Default time before a turn decision at which waiting heads start their
/// air checks (the glue sets it from the probe duration).
constexpr uint32_t kCheckLeadS = 180;
/// How long past a decision the coordinator waits for air checks still
/// running before handing over anyway.
constexpr uint32_t kCheckWaitS = 300;

/// Does a waiting head's room need its turn? Older firmware doesn't say: Yes.
enum class Need : uint8_t { Yes, No, Checking };
const char* need_str(Need n);               ///< "yes" | "no" | "checking"
Need        parse_need(const std::string& s);

/// Clamp into supported ranges (max slice is never below min slice).
Config sanitize(const Config& c);

/// Compact wire/NVS form "e,min,max,settle" (e = 0/1). decode_config()
/// rejects malformed input and sanitizes what it accepts.
std::string encode_config(const Config& c);
bool        decode_config(const std::string& s, Config& out);

/// The published slice state.
struct State {
    uint64_t    term = 0;     ///< bumped each time a head becomes coordinator
    uint64_t    seq  = 0;     ///< bumped on every phase/mode change
    Mode        mode = Mode::None;
    Phase       phase = Phase::Idle;
    uint32_t    elapsed_s = 0;  ///< time in this phase (defrost excluded in Run)
    std::string coordinator;
    /// Run: elapsed time at which the turn is next reconsidered (0 = the max
    /// turn). Pushed out by a max turn each time the waiting rooms turn out
    /// not to need theirs.
    uint32_t    decide_s = 0;
    /// Run: waiting heads should air-check their rooms now (a decision is
    /// near, or the turn's rooms are all satisfied).
    bool        check = false;
};

/// (a.term, a.seq) >= (b.term, b.seq).
bool not_older(const State& a, const State& b);

/// One group member as seen by this head. By convention members[0] is self.
struct Member {
    std::string uid;
    bool  fresh = false;          ///< self, or a peer with a fresh signed poll
    bool  capable = false;        ///< advertises kSliceVersion
    Mode  wants = Mode::None;     ///< desired direction (power + mode)
    bool  satisfied = false;      ///< parked at setpoint (#132)
    bool  defrost = false;        ///< reports DEFROST
    Need  need = Need::Yes;       ///< while waiting: does its room need a turn
    bool  has_state = false;      ///< published a slice State
    State state;
};

/// Lowest uid among fresh, capable members ("" if none).
std::string elect(const std::vector<Member>& members);

/// What a head should do with its desired mode right now.
enum class Directive {
    Free,  ///< not slicing (disabled / lost coordinator / FAN / OFF): normal behaviour
    Run,   ///< its mode's slice: run with normal parking
    Hold,  ///< another slice, a changeover, or waiting to be scheduled: stay parked
};
const char* directive_str(Directive d);   ///< "free" | "run" | "hold"

class Node {
public:
    void set_self(const std::string& uid) { self_ = uid; }
    void configure(const Config& c, uint32_t now_s);
    const Config& config() const { return cfg_; }
    /// Lead time for waiting heads' air checks before a decision.
    void set_check_lead(uint32_t s) { check_lead_ = s; }

    /// Restore the highest term seen (persisted) so a rebooted head never
    /// reuses an old term.
    void restore_term(uint64_t t) { if (t > max_term_) max_term_ = t; }
    uint64_t max_term() const { return max_term_; }

    /// Advance ~1 Hz. @p members[0] must be this head.
    void tick(const std::vector<Member>& members, uint32_t now_s);

    bool enabled() const { return cfg_.enabled; }
    bool warming_up(uint32_t now_s) const;
    bool is_coordinator() const { return cfg_.enabled && coord_; }
    /// The head currently elected coordinator ("" if none).
    const std::string& coordinator_uid() const { return coord_uid_; }
    /// A fresh slice state is in force.
    bool have_state() const { return cfg_.enabled && have_state_; }
    /// The state this head publishes (coordinator: authoritative; follower:
    /// last accepted).
    const State& state() const { return state_; }
    /// Time in the current phase, extrapolated since the last update.
    uint32_t elapsed_now(uint32_t now_s) const;

    Directive directive(Mode wants, uint32_t now_s) const;

    /// A slice transition worth an activity-log entry (coordinator only), or
    /// nullptr. Clears it.
    const char* take_event();

private:
    void schedule(const std::vector<Member>& members, uint32_t dt);
    void enter(Phase p, Mode m);
    void take_over(const std::vector<Member>& members);
    void push(const char* e);

    std::string self_;
    Config   cfg_{};
    uint32_t check_lead_ = kCheckLeadS;
    State    state_{};
    bool     coord_ = false;
    std::string coord_uid_;
    bool     have_state_ = false;
    uint32_t updated_at_ = 0;   ///< when state_'s content last changed
    uint32_t last_seen_ = 0;    ///< follower: last tick the coordinator's state was seen
    uint64_t max_term_ = 0;
    uint32_t enabled_at_ = 0;
    bool     have_tick_ = false;
    uint32_t last_tick_ = 0;
    std::vector<const char*> events_;
};

}  // namespace slice
