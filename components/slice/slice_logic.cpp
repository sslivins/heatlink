// Time-sliced heat/cool scheduler (#133). See slice_logic.h.

#include "slice_logic.h"

#include <cstdio>
#include <cstdlib>

namespace slice {

const char* mode_str(Mode m) {
    switch (m) {
        case Mode::Heat: return "heat";
        case Mode::Cool: return "cool";
        default:         return "none";
    }
}

Mode parse_mode(const std::string& s) {
    if (s == "heat") return Mode::Heat;
    if (s == "cool") return Mode::Cool;
    return Mode::None;
}

Mode other(Mode m) {
    if (m == Mode::Heat) return Mode::Cool;
    if (m == Mode::Cool) return Mode::Heat;
    return Mode::None;
}

Mode desired_of(bool power_on, const std::string& mode) {
    if (!power_on) return Mode::None;
    if (mode == "HEAT") return Mode::Heat;
    if (mode == "COOL" || mode == "DRY") return Mode::Cool;
    return Mode::None;
}

const char* phase_str(Phase p) {
    switch (p) {
        case Phase::Run:        return "run";
        case Phase::Changeover: return "changeover";
        default:                return "idle";
    }
}

Phase parse_phase(const std::string& s) {
    if (s == "run") return Phase::Run;
    if (s == "changeover") return Phase::Changeover;
    return Phase::Idle;
}

const char* directive_str(Directive d) {
    switch (d) {
        case Directive::Run:  return "run";
        case Directive::Hold: return "hold";
        default:              return "free";
    }
}

static uint32_t clampu(uint32_t v, uint32_t lo, uint32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

Config sanitize(const Config& c) {
    Config s = c;
    s.min_slice_s = clampu(c.min_slice_s, kMinSliceFloorS, kMinSliceCapS);
    s.max_slice_s = clampu(c.max_slice_s, s.min_slice_s, kMaxSliceCapS);
    s.settle_s    = clampu(c.settle_s, kMinSettleS, kMaxSettleS);
    return s;
}

std::string encode_config(const Config& c) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%d,%u,%u,%u", c.enabled ? 1 : 0,
                  static_cast<unsigned>(c.min_slice_s),
                  static_cast<unsigned>(c.max_slice_s),
                  static_cast<unsigned>(c.settle_s));
    return buf;
}

bool decode_config(const std::string& s, Config& out) {
    if (s.empty() || s.size() > 32) return false;
    unsigned long v[4];
    size_t pos = 0;
    for (int i = 0; i < 4; ++i) {
        size_t start = pos;
        while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') ++pos;
        if (pos == start || pos - start > 6) return false;
        v[i] = std::strtoul(s.substr(start, pos - start).c_str(), nullptr, 10);
        if (i < 3) {
            if (pos >= s.size() || s[pos] != ',') return false;
            ++pos;
        }
    }
    if (pos != s.size() || v[0] > 1) return false;
    Config c;
    c.enabled = v[0] == 1;
    c.min_slice_s = static_cast<uint32_t>(v[1]);
    c.max_slice_s = static_cast<uint32_t>(v[2]);
    c.settle_s = static_cast<uint32_t>(v[3]);
    out = sanitize(c);
    return true;
}

bool not_older(const State& a, const State& b) {
    if (a.term != b.term) return a.term > b.term;
    return a.seq >= b.seq;
}

std::string elect(const std::vector<Member>& members) {
    const std::string* best = nullptr;
    for (const auto& m : members) {
        if (!m.fresh || !m.capable || m.uid.empty()) continue;
        if (!best || m.uid < *best) best = &m.uid;
    }
    return best ? *best : std::string();
}

// ── Node ────────────────────────────────────────────────────────────────────

void Node::configure(const Config& c, uint32_t now_s) {
    Config s = sanitize(c);
    if (s.enabled && !cfg_.enabled) enabled_at_ = now_s;
    if (!s.enabled) {
        coord_ = false;
        coord_uid_.clear();
        have_state_ = false;
        state_ = State{};
        events_.clear();
    }
    cfg_ = s;
}

bool Node::warming_up(uint32_t now_s) const {
    return cfg_.enabled && now_s - enabled_at_ < kWarmupS;
}

uint32_t Node::elapsed_now(uint32_t now_s) const {
    if (coord_) return state_.elapsed_s;
    return state_.elapsed_s + (now_s - updated_at_);
}

const char* Node::take_event() {
    if (events_.empty()) return nullptr;
    const char* e = events_.front();
    events_.erase(events_.begin());
    return e;
}

void Node::push(const char* e) {
    if (events_.size() < 8) events_.push_back(e);
}

void Node::enter(Phase p, Mode m) {
    if (p == state_.phase && m == state_.mode) return;
    state_.phase = p;
    state_.mode = m;
    state_.elapsed_s = 0;
    ++state_.seq;
    if (p == Phase::Run) {
        push(m == Mode::Heat ? "Tandem: heating turn started"
                                 : "Tandem: cooling turn started");
    } else if (p == Phase::Changeover) {
        push(m == Mode::Heat ? "Tandem: changing over to heating"
                                 : "Tandem: changing over to cooling");
    } else {
        push("Tandem: idle (no heat/cool demand)");
    }
}

// Become coordinator: continue the newest schedule any peer knows about
// under a fresh term.
void Node::take_over(const std::vector<Member>& members) {
    const State* best = nullptr;
    for (size_t i = 1; i < members.size(); ++i) {
        const Member& m = members[i];
        if (!m.fresh || !m.has_state) continue;
        if (!best || !not_older(*best, m.state)) best = &m.state;
    }
    if (best) {
        state_.mode = best->mode;
        state_.phase = best->phase;
        state_.elapsed_s = best->elapsed_s;
    } else if (!have_state_) {
        state_ = State{};
    }
    state_.term = max_term_ + 1;
    state_.seq = 0;
    state_.coordinator = self_;
    max_term_ = state_.term;
    coord_ = true;
    push("Tandem: this head is now the coordinator");
}

void Node::tick(const std::vector<Member>& members, uint32_t now_s) {
    uint32_t dt = have_tick_ ? now_s - last_tick_ : 0;
    if (dt > 60) dt = 60;
    have_tick_ = true;
    last_tick_ = now_s;
    if (!cfg_.enabled || members.empty()) return;

    for (const auto& m : members) {
        if (m.fresh && m.has_state && m.state.term > max_term_) max_term_ = m.state.term;
    }

    const std::string elected = elect(members);
    const bool warm = warming_up(now_s);

    if (elected == self_ && !warm) {
        if (!coord_) {
            take_over(members);
        } else {
            // Another head published a newer term (e.g. we were partitioned
            // and someone else coordinated): adopt its schedule, outrank it.
            for (size_t i = 1; i < members.size(); ++i) {
                const Member& m = members[i];
                if (!m.fresh || !m.has_state || m.state.term <= state_.term) continue;
                state_.mode = m.state.mode;
                state_.phase = m.state.phase;
                state_.elapsed_s = m.state.elapsed_s;
                state_.term = max_term_ + 1;
                state_.seq = 0;
                max_term_ = state_.term;
            }
        }
        coord_uid_ = self_;
        schedule(members, dt);
        have_state_ = true;
        updated_at_ = now_s;
        return;
    }

    if (coord_) {
        // Stepping down: keep serving our own schedule until the new
        // coordinator publishes one (or kStaleS passes).
        coord_ = false;
        last_seen_ = now_s;
        updated_at_ = now_s;
    }
    // Follow the elected coordinator; while warming up as the would-be
    // coordinator, follow the newest schedule a peer knows about instead.
    const State* cand = nullptr;
    for (size_t i = 1; i < members.size(); ++i) {
        const Member& m = members[i];
        if (!m.fresh || !m.has_state) continue;
        if (elected != self_) {
            if (m.uid == elected && m.state.coordinator == elected) cand = &m.state;
        } else if (!cand || !not_older(*cand, m.state)) {
            cand = &m.state;
        }
    }
    coord_uid_ = cand ? cand->coordinator : elected;
    if (cand) {
        // A coordinator change can legitimately go "backwards" if the new
        // one never saw our term: accept it once ours has gone stale.
        const bool stale = !have_state_ || now_s - last_seen_ > kStaleS;
        if (stale || not_older(*cand, state_)) {
            if (stale || cand->term != state_.term || cand->seq != state_.seq ||
                cand->elapsed_s != state_.elapsed_s) {
                state_ = *cand;
                updated_at_ = now_s;
            }
            have_state_ = true;
            last_seen_ = now_s;
        }
    }
    if (have_state_ && now_s - last_seen_ > kStaleS) have_state_ = false;
}

void Node::schedule(const std::vector<Member>& members, uint32_t dt) {
    bool want_heat = false, want_cool = false, defrost = false;
    for (const auto& m : members) {
        if (!m.fresh) continue;
        if (m.wants == Mode::Heat) want_heat = true;
        if (m.wants == Mode::Cool) want_cool = true;
        if (m.defrost) defrost = true;
    }
    auto demand = [&](Mode m) {
        return m == Mode::Heat ? want_heat : (m == Mode::Cool ? want_cool : false);
    };
    auto all_satisfied = [&](Mode md) {
        for (const auto& m : members) {
            if (m.fresh && m.wants == md && !m.satisfied) return false;
        }
        return true;
    };

    const Mode cur = state_.mode;
    switch (state_.phase) {
        case Phase::Idle: {
            state_.elapsed_s += dt;
            if (!want_heat && !want_cool) return;
            Mode next = (cur != Mode::None && demand(cur)) ? cur
                        : (want_heat ? Mode::Heat : Mode::Cool);
            // The unit only just stopped running the other way: let it settle.
            if (cur != Mode::None && next != cur && state_.elapsed_s < cfg_.settle_s) {
                enter(Phase::Changeover, next);
            } else {
                enter(Phase::Run, next);
            }
            return;
        }
        case Phase::Run: {
            if (!defrost) state_.elapsed_s += dt;
            const Mode opp = other(cur);
            if (!demand(cur) && !demand(opp)) {
                enter(Phase::Idle, cur);
                return;
            }
            if (defrost) return;  // never switch mid-defrost
            if (!demand(cur)) {
                enter(Phase::Changeover, opp);
                return;
            }
            if (demand(opp) &&
                (state_.elapsed_s >= cfg_.max_slice_s ||
                 (state_.elapsed_s >= cfg_.min_slice_s && all_satisfied(cur)))) {
                enter(Phase::Changeover, opp);
            }
            return;
        }
        case Phase::Changeover: {
            state_.elapsed_s += dt;
            if (state_.elapsed_s < cfg_.settle_s) return;
            if (demand(cur)) {
                enter(Phase::Run, cur);
            } else if (demand(other(cur))) {
                enter(Phase::Run, other(cur));
            } else {
                enter(Phase::Idle, cur);
            }
            return;
        }
    }
}

Directive Node::directive(Mode wants, uint32_t now_s) const {
    if (!cfg_.enabled || wants == Mode::None) return Directive::Free;
    if (!have_state_) return warming_up(now_s) ? Directive::Hold : Directive::Free;
    switch (state_.phase) {
        case Phase::Run:
            return wants == state_.mode ? Directive::Run : Directive::Hold;
        default:
            // Changeover, or Idle until the coordinator schedules our demand.
            return Directive::Hold;
    }
}

}  // namespace slice
