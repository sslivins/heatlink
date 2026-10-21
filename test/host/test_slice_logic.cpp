// Host-side unit tests for the time-slice scheduler (slice_logic.cpp, #133).
//
// Build & run (see .github/workflows/host-tests.yml):
//   g++ -std=c++17 -Wall -Wextra -I components/slice/include
//       components/slice/slice_logic.cpp test/host/test_slice_logic.cpp

#include "slice_logic.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_failures = 0;

#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);       \
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

using namespace slice;

namespace {

Config on_cfg() {
    Config c;
    c.enabled = true;
    return c;
}

Member mem(const std::string& uid, Mode wants, bool satisfied = false, bool defrost = false) {
    Member m;
    m.uid = uid;
    m.fresh = true;
    m.capable = true;
    m.wants = wants;
    m.satisfied = satisfied;
    m.defrost = defrost;
    return m;
}

Member with_state(Member m, const State& s) {
    m.has_state = true;
    m.state = s;
    return m;
}

State st(uint64_t term, uint64_t seq, Mode mode, Phase phase, const std::string& coord,
         uint32_t elapsed = 0) {
    State s;
    s.term = term;
    s.seq = seq;
    s.mode = mode;
    s.phase = phase;
    s.coordinator = coord;
    s.elapsed_s = elapsed;
    return s;
}

// Tick @p n over [from, to).
void run(Node& n, const std::vector<Member>& ms, uint32_t from, uint32_t to) {
    for (uint32_t t = from; t < to; ++t) n.tick(ms, t);
}

// Coordinator "a" past warm-up, with self wanting @p a_wants and peer "b"
// wanting @p b_wants. Returns the node at t = kWarmupS.
Node coordinator(Mode a_wants, Mode b_wants, uint32_t& now) {
    Node n;
    n.set_self("a");
    n.configure(on_cfg(), 0);
    run(n, {mem("a", a_wants), mem("b", b_wants)}, 0, kWarmupS + 1);
    now = kWarmupS + 1;
    return n;
}

void drain(Node& n) {
    while (n.take_event()) {}
}

}  // namespace

static void test_config() {
    Config c;
    c.min_slice_s = 10;
    c.max_slice_s = 5;
    c.settle_s = 9999;
    Config s = sanitize(c);
    CHECK(s.min_slice_s == kMinSliceFloorS);
    CHECK(s.max_slice_s == kMinSliceFloorS);
    CHECK(s.settle_s == kMaxSettleS);

    Config e = on_cfg();
    e.min_slice_s = 900;
    CHECK(encode_config(e) == "1,900,1800,180");
    Config d;
    CHECK(decode_config("1,900,1800,240", d));
    CHECK(d.enabled && d.min_slice_s == 900 && d.max_slice_s == 1800 && d.settle_s == 240);
    // A pause saved by older firmware is raised to the 3 min floor.
    CHECK(decode_config("1,900,1800,30", d) && d.settle_s == kMinSettleS);
    CHECK(decode_config("0,1,1,1", d) && !d.enabled && d.min_slice_s == kMinSliceFloorS);
    CHECK(!decode_config("", d));
    CHECK(!decode_config("2,600,1800,60", d));
    CHECK(!decode_config("1,600,1800", d));
    CHECK(!decode_config("1,600,1800,60,", d));
    CHECK(!decode_config("1,-600,1800,60", d));
    CHECK(!decode_config("1,6000000,1800,60", d));
}

static void test_desired_and_elect() {
    CHECK(desired_of(true, "HEAT") == Mode::Heat);
    CHECK(desired_of(true, "COOL") == Mode::Cool);
    CHECK(desired_of(true, "DRY") == Mode::Cool);
    CHECK(desired_of(true, "FAN") == Mode::None);
    CHECK(desired_of(true, "AUTO") == Mode::None);
    CHECK(desired_of(false, "HEAT") == Mode::None);

    std::vector<Member> ms = {mem("c", Mode::None), mem("b", Mode::None), mem("a", Mode::None)};
    CHECK(elect(ms) == "a");
    ms[2].fresh = false;
    CHECK(elect(ms) == "b");
    ms[1].capable = false;
    CHECK(elect(ms) == "c");

    CHECK(not_older(st(2, 0, Mode::Heat, Phase::Run, "a"), st(1, 9, Mode::Heat, Phase::Run, "a")));
    CHECK(not_older(st(2, 3, Mode::Heat, Phase::Run, "a"), st(2, 3, Mode::Heat, Phase::Run, "a")));
    CHECK(!not_older(st(2, 2, Mode::Heat, Phase::Run, "a"), st(2, 3, Mode::Heat, Phase::Run, "a")));
}

static void test_warmup_then_coordinate() {
    Node n;
    n.set_self("a");
    n.configure(on_cfg(), 0);
    std::vector<Member> ms = {mem("a", Mode::Heat), mem("b", Mode::Cool)};
    run(n, ms, 0, 10);
    CHECK(!n.is_coordinator());
    CHECK(n.directive(Mode::Heat, 10) == Directive::Hold);  // warming up: wait
    CHECK(n.directive(Mode::None, 10) == Directive::Free);  // FAN/OFF never held
    run(n, ms, 10, kWarmupS + 1);
    CHECK(n.is_coordinator());
    CHECK(n.coordinator_uid() == "a");
    CHECK(n.state().term == 1);
    CHECK(n.state().phase == Phase::Run);
    CHECK(n.state().mode == Mode::Heat);  // heat preferred from cold start
    CHECK(n.directive(Mode::Heat, kWarmupS) == Directive::Run);
    CHECK(n.directive(Mode::Cool, kWarmupS) == Directive::Hold);
    CHECK(std::string(n.take_event()) == "Tandem: this head is now the coordinator");
    CHECK(std::string(n.take_event()) == "Tandem: switched to heating");
    CHECK(n.take_event() == nullptr);
}

static void test_max_slice_forces_switch() {
    uint32_t now;
    Node n = coordinator(Mode::Heat, Mode::Cool, now);
    drain(n);
    std::vector<Member> ms = {mem("a", Mode::Heat), mem("b", Mode::Cool)};
    // Heat never satisfied: runs the full max slice.
    run(n, ms, now, now + 1798);
    CHECK(n.state().phase == Phase::Run);
    run(n, ms, now + 1798, now + 1801);
    CHECK(n.state().phase == Phase::Changeover);
    CHECK(n.state().mode == Mode::Cool);
    CHECK(n.directive(Mode::Heat, now) == Directive::Hold);
    CHECK(n.directive(Mode::Cool, now) == Directive::Hold);  // settle first
    run(n, ms, now + 1801, now + 1801 + 170);
    CHECK(n.state().phase == Phase::Changeover);  // 3 min minimum pause
    run(n, ms, now + 1801 + 170, now + 1801 + 181);
    CHECK(n.state().phase == Phase::Run);
    CHECK(n.state().mode == Mode::Cool);
    CHECK(n.directive(Mode::Cool, now) == Directive::Run);
    CHECK(std::string(n.take_event()) == "Tandem: changing over to cooling");
    CHECK(std::string(n.take_event()) == "Tandem: switched to cooling");
}

static void test_satisfied_switches_after_min() {
    uint32_t now;
    Node n = coordinator(Mode::Heat, Mode::Cool, now);
    std::vector<Member> ms = {mem("a", Mode::Heat, /*satisfied=*/true), mem("b", Mode::Cool)};
    run(n, ms, now, now + 590);
    CHECK(n.state().phase == Phase::Run);  // min slice not reached yet
    CHECK(n.state().mode == Mode::Heat);
    run(n, ms, now + 590, now + 610);
    CHECK(n.state().phase == Phase::Changeover);
    CHECK(n.state().mode == Mode::Cool);
}

static void test_no_switch_without_other_demand() {
    uint32_t now;
    Node n = coordinator(Mode::Heat, Mode::Heat, now);
    std::vector<Member> ms = {mem("a", Mode::Heat), mem("b", Mode::Heat)};
    run(n, ms, now, now + 5000);
    CHECK(n.state().phase == Phase::Run);
    CHECK(n.state().mode == Mode::Heat);
}

static void test_defrost_blocks_switch_and_pauses_clock() {
    uint32_t now;
    Node n = coordinator(Mode::Heat, Mode::Cool, now);
    std::vector<Member> ms = {mem("a", Mode::Heat, false, /*defrost=*/true), mem("b", Mode::Cool)};
    run(n, ms, now, now + 4000);
    CHECK(n.state().phase == Phase::Run);
    CHECK(n.state().mode == Mode::Heat);
    CHECK(n.state().elapsed_s < 5);
    // Defrost ends: the clock resumes.
    ms[0].defrost = false;
    run(n, ms, now + 4000, now + 4100);
    CHECK(n.state().phase == Phase::Run);
    CHECK(n.state().elapsed_s >= 99);
}

static void test_leaving_mode_switches_immediately() {
    uint32_t now;
    Node n = coordinator(Mode::Heat, Mode::Cool, now);
    // The only heat head is switched off: change over without waiting for min.
    std::vector<Member> ms = {mem("a", Mode::None), mem("b", Mode::Cool)};
    run(n, ms, now, now + 2);
    CHECK(n.state().phase == Phase::Changeover);
    CHECK(n.state().mode == Mode::Cool);
}

static void test_idle_and_resume() {
    uint32_t now;
    Node n = coordinator(Mode::Cool, Mode::None, now);
    CHECK(n.state().mode == Mode::Cool);
    std::vector<Member> none = {mem("a", Mode::None), mem("b", Mode::None)};
    run(n, none, now, now + 5);
    CHECK(n.state().phase == Phase::Idle);
    CHECK(n.directive(Mode::Heat, now + 5) == Directive::Hold);  // until scheduled
    // Heat wanted soon after cooling stopped: settle via a changeover.
    std::vector<Member> heat = {mem("a", Mode::None), mem("b", Mode::Heat)};
    run(n, heat, now + 5, now + 7);
    CHECK(n.state().phase == Phase::Changeover);
    CHECK(n.state().mode == Mode::Heat);
    // After a long idle no settle is needed before switching direction.
    Node m = coordinator(Mode::Cool, Mode::None, now);
    run(m, none, now, now + 5);
    run(m, none, now + 5, now + 500);
    std::vector<Member> heat2 = {mem("a", Mode::Heat), mem("b", Mode::None)};
    run(m, heat2, now + 500, now + 502);
    CHECK(m.state().phase == Phase::Run);
    CHECK(m.state().mode == Mode::Heat);
}

static void test_changeover_retargets_without_demand() {
    uint32_t now;
    Node n = coordinator(Mode::Heat, Mode::Cool, now);
    std::vector<Member> ms = {mem("a", Mode::None), mem("b", Mode::Cool)};
    run(n, ms, now, now + 2);
    CHECK(n.state().phase == Phase::Changeover);
    // Cool demand vanishes mid-changeover while heat comes back.
    std::vector<Member> back = {mem("a", Mode::Heat), mem("b", Mode::None)};
    run(n, back, now + 2, now + 200);
    CHECK(n.state().phase == Phase::Run);
    CHECK(n.state().mode == Mode::Heat);
}

static void test_follower_accepts_ordered_states() {
    Node n;
    n.set_self("b");
    n.configure(on_cfg(), 0);
    State s1 = st(2, 3, Mode::Cool, Phase::Run, "a", 100);
    n.tick({mem("b", Mode::Heat), with_state(mem("a", Mode::Cool), s1)}, 100);
    CHECK(!n.is_coordinator());
    CHECK(n.coordinator_uid() == "a");
    CHECK(n.have_state());
    CHECK(n.state().mode == Mode::Cool);
    CHECK(n.directive(Mode::Heat, 100) == Directive::Hold);
    CHECK(n.directive(Mode::Cool, 100) == Directive::Run);
    CHECK(n.elapsed_now(110) == 110);
    CHECK(n.take_event() == nullptr);  // only the coordinator logs

    // Older (term, seq) from the same coordinator is ignored.
    State old = st(2, 2, Mode::Heat, Phase::Run, "a");
    n.tick({mem("b", Mode::Heat), with_state(mem("a", Mode::Cool), old)}, 101);
    CHECK(n.state().mode == Mode::Cool);
    // A non-elected peer's newer-looking state is ignored too.
    State rogue = st(9, 0, Mode::Heat, Phase::Run, "c");
    n.tick({mem("b", Mode::Heat), with_state(mem("a", Mode::Cool), s1),
            with_state(mem("c", Mode::Heat), rogue)},
           102);
    CHECK(n.state().mode == Mode::Cool);
    CHECK(n.max_term() == 9);
    // Newer state is accepted.
    State s2 = st(2, 4, Mode::Heat, Phase::Changeover, "a");
    n.tick({mem("b", Mode::Heat), with_state(mem("a", Mode::Cool), s2)}, 103);
    CHECK(n.state().phase == Phase::Changeover);
    CHECK(n.directive(Mode::Heat, 103) == Directive::Hold);
}

static void test_failover_continues_schedule() {
    Node n;
    n.set_self("b");
    n.configure(on_cfg(), 0);
    State s = st(4, 7, Mode::Cool, Phase::Run, "a", 300);
    std::vector<Member> ms = {mem("b", Mode::Heat), with_state(mem("a", Mode::Cool), s),
                              mem("c", Mode::Cool)};
    run(n, ms, 0, 40);
    CHECK(!n.is_coordinator());
    drain(n);
    // "a" disappears: "b" is now the lowest uid and takes over under a new
    // term, continuing the cool slice rather than restarting.
    ms[1].fresh = false;
    n.tick(ms, 40);
    CHECK(n.is_coordinator());
    CHECK(n.state().term == 5);
    CHECK(n.state().mode == Mode::Cool);
    CHECK(n.state().phase == Phase::Run);
    CHECK(n.state().elapsed_s >= 300);
    CHECK(std::string(n.take_event()) == "Tandem: this head is now the coordinator");
}

static void test_stale_coordinator_converges() {
    // "a" was partitioned and kept coordinating at term 2; "b" took over at
    // term 5. When "a" is reachable again it is elected but must adopt the
    // newer schedule and outrank it.
    uint32_t now;
    Node a = coordinator(Mode::Heat, Mode::Cool, now);
    CHECK(a.state().term == 1);
    State b_state = st(5, 2, Mode::Cool, Phase::Run, "b", 50);
    a.tick({mem("a", Mode::Heat), with_state(mem("b", Mode::Cool), b_state)}, now);
    CHECK(a.is_coordinator());
    CHECK(a.state().term == 6);
    CHECK(a.state().mode == Mode::Cool);

    // Meanwhile "b" steps down: it keeps its own schedule until "a" publishes
    // the newer term, ignoring a's stale term-1 state.
    Node b;
    b.set_self("b");
    b.restore_term(4);  // it had followed a's later terms before the split
    b.configure(on_cfg(), 0);
    std::vector<Member> solo = {mem("b", Mode::Cool), mem("a", Mode::Heat)};
    solo[1].fresh = false;
    run(b, solo, 0, 40);
    CHECK(b.is_coordinator());
    CHECK(b.state().mode == Mode::Cool);
    State a_stale = st(1, 9, Mode::Heat, Phase::Run, "a");
    std::vector<Member> both = {mem("b", Mode::Cool), with_state(mem("a", Mode::Heat), a_stale)};
    b.tick(both, 40);
    CHECK(!b.is_coordinator());
    CHECK(b.have_state());
    CHECK(b.state().mode == Mode::Cool);
    CHECK(b.directive(Mode::Cool, 40) == Directive::Run);
    State a_new = st(6, 0, Mode::Cool, Phase::Run, "a");
    both[1].state = a_new;
    b.tick(both, 50);
    CHECK(b.state().term == 6);
    CHECK(b.state().coordinator == "a");
}

static void test_lost_coordinator_falls_back() {
    Node n;
    n.set_self("b");
    n.configure(on_cfg(), 0);
    State s = st(1, 1, Mode::Cool, Phase::Run, "a");
    run(n, {mem("b", Mode::Heat), with_state(mem("a", Mode::Cool), s)}, 0, 40);
    CHECK(n.directive(Mode::Heat, 40) == Directive::Hold);
    // "a" stays reachable but stops publishing a state (e.g. it is
    // rebooting into warm-up): keep the last state for kStaleS, then run free.
    std::vector<Member> ms = {mem("b", Mode::Heat), mem("a", Mode::Cool)};
    run(n, ms, 40, 40 + kStaleS);
    CHECK(n.have_state());
    run(n, ms, 40 + kStaleS, 40 + kStaleS + 2);
    CHECK(!n.have_state());
    CHECK(n.directive(Mode::Heat, 90) == Directive::Free);
}

static void test_warmup_follows_peers_then_takes_over() {
    // "a" (lowest uid) reboots while "b" coordinates term 5: during warm-up it
    // follows b's schedule, then takes over at term 6 without restarting it.
    Node a;
    a.set_self("a");
    a.restore_term(3);
    a.configure(on_cfg(), 0);
    State s = st(5, 2, Mode::Cool, Phase::Run, "b", 200);
    std::vector<Member> ms = {mem("a", Mode::Heat), with_state(mem("b", Mode::Cool), s)};
    a.tick(ms, 0);
    CHECK(!a.is_coordinator());
    CHECK(a.coordinator_uid() == "b");
    CHECK(a.directive(Mode::Heat, 0) == Directive::Hold);
    CHECK(a.directive(Mode::Cool, 0) == Directive::Run);
    run(a, ms, 1, kWarmupS + 1);
    CHECK(a.is_coordinator());
    CHECK(a.state().term == 6);
    CHECK(a.state().mode == Mode::Cool);
    CHECK(a.state().phase == Phase::Run);
}

static void test_disable_frees_everyone() {
    uint32_t now;
    Node n = coordinator(Mode::Heat, Mode::Cool, now);
    CHECK(n.directive(Mode::Cool, now) == Directive::Hold);
    n.configure(Config{}, now);
    CHECK(!n.is_coordinator());
    CHECK(!n.have_state());
    CHECK(n.directive(Mode::Cool, now) == Directive::Free);
    CHECK(n.take_event() == nullptr);
    CHECK(n.max_term() == 1);  // terms never go backwards
}

static Member needing(Member m, Need n) {
    m.need = n;
    return m;
}

// #133 need check: at the max turn the waiting rooms are asked; if none needs
// its turn the current one is extended by another max turn.
static void test_max_extends_when_not_needed() {
    uint32_t now;
    Node n = coordinator(Mode::Heat, Mode::Cool, now);
    drain(n);
    n.set_check_lead(180);
    std::vector<Member> ms = {mem("a", Mode::Heat), needing(mem("b", Mode::Cool), Need::No)};
    run(n, ms, now, now + 1500);
    CHECK(!n.state().check);
    run(n, ms, now + 1500, now + 1700);
    CHECK(n.state().check);                    // within the lead: waiting heads check
    run(n, ms, now + 1700, now + 1900);
    CHECK(n.state().phase == Phase::Run);
    CHECK(n.state().mode == Mode::Heat);
    CHECK(n.state().decide_s == 3600);
    CHECK(!n.state().check);
    CHECK(std::string(n.take_event()) ==
          "Tandem: kept heating \xE2\x80\x94 rooms waiting to cool don't need it yet");
    // Next decision: the cooling room now needs it.
    ms[1].need = Need::Yes;
    run(n, ms, now + 1900, now + 3590);
    CHECK(n.state().phase == Phase::Run);
    run(n, ms, now + 3590, now + 3610);
    CHECK(n.state().phase == Phase::Changeover);
    CHECK(n.state().mode == Mode::Cool);
    CHECK(n.state().decide_s == 0);
}

static void test_waits_for_running_check() {
    uint32_t now;
    Node n = coordinator(Mode::Heat, Mode::Cool, now);
    std::vector<Member> ms = {mem("a", Mode::Heat), needing(mem("b", Mode::Cool), Need::Checking)};
    run(n, ms, now, now + 1800 + kCheckWaitS - 10);
    CHECK(n.state().phase == Phase::Run);      // waits for the verdict
    CHECK(n.state().decide_s == 0);            // not extended while checking
    ms[1].need = Need::Yes;
    run(n, ms, now + 1800 + kCheckWaitS - 10, now + 1800 + kCheckWaitS - 8);
    CHECK(n.state().phase == Phase::Changeover);
    // A check that never finishes hands over after kCheckWaitS.
    Node m = coordinator(Mode::Heat, Mode::Cool, now);
    ms[1].need = Need::Checking;
    run(m, ms, now, now + 1800 + kCheckWaitS + 5);
    CHECK(m.state().phase == Phase::Changeover);
}

static void test_satisfied_needs_waiting_demand() {
    uint32_t now;
    Node n = coordinator(Mode::Heat, Mode::Cool, now);
    std::vector<Member> ms = {mem("a", Mode::Heat, /*satisfied=*/true),
                              needing(mem("b", Mode::Cool), Need::No)};
    run(n, ms, now, now + 590);
    CHECK(!n.state().check);
    run(n, ms, now + 590, now + 1000);
    CHECK(n.state().phase == Phase::Run);      // nobody waiting needs it
    CHECK(n.state().mode == Mode::Heat);
    CHECK(n.state().check);                    // keep the waiting rooms checked
    ms[1].need = Need::Yes;
    run(n, ms, now + 1000, now + 1002);
    CHECK(n.state().phase == Phase::Changeover);
}

static void test_follower_gets_check() {
    Node f;
    f.set_self("b");
    f.configure(on_cfg(), 0);
    State s = st(1, 1, Mode::Heat, Phase::Run, "a", 1700);
    s.check = true;
    s.decide_s = 3600;
    std::vector<Member> ms = {mem("b", Mode::Cool), with_state(mem("a", Mode::Heat), s)};
    run(f, ms, 0, 5);
    CHECK(f.have_state());
    CHECK(f.state().check);
    CHECK(f.state().decide_s == 3600);
    s.check = false;
    ms[1] = with_state(mem("a", Mode::Heat), s);
    run(f, ms, 5, 6);
    CHECK(!f.state().check);
}

int main() {
    test_config();
    test_desired_and_elect();
    test_warmup_then_coordinate();
    test_max_slice_forces_switch();
    test_satisfied_switches_after_min();
    test_no_switch_without_other_demand();
    test_defrost_blocks_switch_and_pauses_clock();
    test_leaving_mode_switches_immediately();
    test_idle_and_resume();
    test_changeover_retargets_without_demand();
    test_follower_accepts_ordered_states();
    test_failover_continues_schedule();
    test_stale_coordinator_converges();
    test_lost_coordinator_falls_back();
    test_warmup_follows_peers_then_takes_over();
    test_disable_frees_everyone();
    test_max_extends_when_not_needed();
    test_waits_for_running_check();
    test_satisfied_needs_waiting_demand();
    test_follower_gets_check();
    if (g_failures) {
        std::printf("%d slice_logic test(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("All slice_logic tests passed\n");
    return 0;
}
