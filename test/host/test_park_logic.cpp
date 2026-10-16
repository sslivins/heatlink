// Host-side unit tests for the off-at-setpoint state machine (park_logic.cpp).
//
// Build & run (see .github/workflows/host-tests.yml):
//   g++ -std=c++17 -Wall -Wextra -I components/park/include
//       components/park/park_logic.cpp test/host/test_park_logic.cpp

#include "park_logic.h"

#include <cstdio>
#include <string>

static int g_failures = 0;

#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);       \
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

using namespace park;

namespace {

Config enabled_cfg() {
    Config c;
    c.enabled = true;
    return c;
}

Inputs heat(float room, float setpoint = 21.0f, bool on = true, bool operating = false,
            const char* sub = "NORMAL") {
    Inputs in;
    in.connected = true;
    in.phys_on = on;
    in.mode = "HEAT";
    in.setpoint = setpoint;
    in.room = room;
    in.operating = operating;
    in.sub_mode = sub;
    return in;
}

// Tick once per second over [from, to); return the first non-None action and
// the second it happened at (or None / -1).
struct Hit { Action a = Action::None; long at = -1; };
Hit run(Machine& m, const Inputs& in, uint32_t from, uint32_t to) {
    for (uint32_t t = from; t < to; ++t) {
        Action a = m.tick(in, t);
        if (a != Action::None) return {a, static_cast<long>(t)};
    }
    return {};
}

// Bring a head up to Running in heat at t=0..1 with the feature on.
Machine running_heat(uint32_t& now) {
    Machine m;
    m.configure(enabled_cfg(), 0);
    m.tick(heat(19.0f), 0);  // adopt: unit on -> Running
    now = 1;
    return m;
}

// Park a running heat head; returns the time it parked.
uint32_t park_it(Machine& m, uint32_t now) {
    Hit h = run(m, heat(21.0f), now, now + 200);
    return static_cast<uint32_t>(h.at);
}

}  // namespace

static void test_sanitize() {
    Config c;
    c.delta_c = 0.2f;
    c.min_off_s = 10;
    c.probe_interval_s = 60;
    c.probe_duration_s = 5;
    Config s = sanitize(c);
    CHECK(s.delta_c == 0.5f);
    CHECK(s.min_off_s == kMinOffFloorS);
    CHECK(s.probe_interval_s == s.min_off_s);  // never shorter than min off
    CHECK(s.probe_duration_s == kMinProbeDurationS);

    c.delta_c = 1.3f;
    c.min_off_s = 900;
    c.probe_interval_s = 600;
    c.probe_duration_s = 9999;
    s = sanitize(c);
    CHECK(s.delta_c == 1.5f);
    CHECK(s.probe_interval_s == 900);
    CHECK(s.probe_duration_s == kMaxProbeDurationS);

    c.delta_c = 9.0f;
    c.min_off_s = 99999;
    s = sanitize(c);
    CHECK(s.delta_c == kMaxDeltaC);
    CHECK(s.min_off_s == kMaxOffS);
}

static void test_disabled_is_passthrough() {
    Machine m;
    CHECK(m.set_desired_power(true, 0) == Action::PowerOn);
    CHECK(run(m, heat(25.0f), 1, 400).a == Action::None);  // never parks
    CHECK(!m.parked());
    CHECK(m.set_desired_power(false, 400) == Action::PowerOff);
    CHECK(m.next_probe_s(400) == -1);
}

static void test_parks_after_debounce() {
    uint32_t now;
    Machine m = running_heat(now);
    CHECK(m.state() == State::Running);
    Hit h = run(m, heat(21.0f), now, now + 200);
    CHECK(h.a == Action::PowerOff);
    CHECK(h.at == static_cast<long>(now + kSatisfiedS));
    CHECK(m.parked());
    CHECK(m.managed());
    const char* ev = m.take_event();
    CHECK(ev != nullptr && std::string(ev) == "Parked at setpoint");
    CHECK(m.take_event() == nullptr);
}

static void test_no_park_when_not_satisfied() {
    uint32_t now;
    Machine m = running_heat(now);
    // Thermo-off but room below setpoint (e.g. blocked by the outdoor unit).
    CHECK(run(m, heat(20.5f), now, now + 300).a == Action::None);
    // Compressor running.
    CHECK(run(m, heat(21.5f, 21.0f, true, true), now + 300, now + 600).a == Action::None);
    // Fan-held / blocked sub-modes never count as satisfied.
    for (const char* sub : {"DEFROST", "PREHEAT", "WARMUP", "STANDBY"})
        CHECK(run(m, heat(21.5f, 21.0f, true, false, sub), now + 600, now + 900).a == Action::None);
    // FAN and AUTO never park.
    Inputs fan = heat(25.0f);
    fan.mode = "FAN";
    CHECK(run(m, fan, now + 900, now + 1200).a == Action::None);
    Inputs aut = heat(25.0f);
    aut.mode = "AUTO";
    CHECK(run(m, aut, now + 1200, now + 1500).a == Action::None);
    CHECK(m.state() == State::Running);
}

static void test_defrost_resets_debounce() {
    uint32_t now;
    Machine m = running_heat(now);
    CHECK(run(m, heat(21.0f), now, now + 50).a == Action::None);
    m.tick(heat(21.0f, 21.0f, true, false, "DEFROST"), now + 50);
    Hit h = run(m, heat(21.0f), now + 51, now + 300);
    CHECK(h.a == Action::PowerOff);
    CHECK(h.at == static_cast<long>(now + 51 + kSatisfiedS));
}

static void test_wake_respects_min_off() {
    uint32_t now;
    Machine m = running_heat(now);
    Config c = enabled_cfg();
    c.probe_enabled = false;
    m.configure(c, now);
    uint32_t p = park_it(m, now);
    // Room 1 °C under setpoint (> delta) but min off not elapsed.
    Inputs cold = heat(20.0f, 21.0f, false);
    Hit h = run(m, cold, p + 1, p + 1000);
    CHECK(h.a == Action::PowerOn);
    CHECK(h.at == static_cast<long>(p + kMinOffFloorS));
    CHECK(m.state() == State::Running);
    const char* ev = m.take_event();
    CHECK(ev != nullptr && std::string(ev).find("drifted") != std::string::npos);
}

static void test_no_wake_inside_delta() {
    uint32_t now;
    Machine m = running_heat(now);
    Config c = enabled_cfg();
    c.probe_enabled = false;
    m.configure(c, now);
    uint32_t p = park_it(m, now);
    CHECK(run(m, heat(20.6f, 21.0f, false), p + 1, p + 3000).a == Action::None);
    CHECK(m.parked());
}

static void test_probe_cycle() {
    uint32_t now;
    Machine m = running_heat(now);
    uint32_t p = park_it(m, now);
    m.take_event();
    CHECK(m.next_probe_s(p) == 900);
    CHECK(m.next_probe_s(p + 600) == 300);

    // Probe starts at the interval.
    Hit h = run(m, heat(21.0f, 21.0f, false), p + 1, p + 2000);
    CHECK(h.a == Action::PowerOn);
    CHECK(h.at == static_cast<long>(p + 900));
    CHECK(m.state() == State::Probing);
    CHECK(m.next_probe_s(h.at) == -1);

    // Preheat (fan held) doesn't count toward the probe's airflow time.
    uint32_t t = h.at + 1;
    CHECK(run(m, heat(21.0f, 21.0f, true, false, "PREHEAT"), t, t + 90).a == Action::None);
    t += 90;
    // Then 120 s of airflow; room satisfied -> re-park without an event.
    h = run(m, heat(21.0f), t, t + 400);
    CHECK(h.a == Action::PowerOff);
    CHECK(h.at >= static_cast<long>(t + 115) && h.at <= static_cast<long>(t + 125));
    CHECK(m.parked());
    CHECK(m.take_event() == nullptr);  // re-parking after a probe is silent
}

static void test_probe_finds_room_cold() {
    uint32_t now;
    Machine m = running_heat(now);
    uint32_t p = park_it(m, now);
    m.take_event();
    Hit h = run(m, heat(20.8f, 21.0f, false), p + 1, p + 2000);  // inside delta: no drift wake
    CHECK(h.a == Action::PowerOn);
    CHECK(m.state() == State::Probing);
    // Real airflow reveals the room is colder than the stagnant reading.
    h = run(m, heat(20.0f), h.at + 1, h.at + 400);
    CHECK(h.a == Action::None);  // already on: no command
    CHECK(m.state() == State::Running);
    const char* ev = m.take_event();
    CHECK(ev != nullptr && std::string(ev).find("probe") != std::string::npos);
}

static void test_probe_cap_without_airflow() {
    uint32_t now;
    Machine m = running_heat(now);
    uint32_t p = park_it(m, now);
    Hit h = run(m, heat(21.0f, 21.0f, false), p + 1, p + 2000);
    CHECK(m.state() == State::Probing);
    // Stuck in defrost: the probe still ends after duration + cap.
    h = run(m, heat(21.0f, 21.0f, true, false, "DEFROST"), h.at + 1, h.at + 2000);
    CHECK(h.a == Action::PowerOff);
    CHECK(m.parked());
}

static void test_cool_and_dry() {
    Machine m;
    m.configure(enabled_cfg(), 0);
    Inputs in = heat(24.0f, 24.0f);
    in.mode = "DRY";
    m.tick(in, 0);
    Hit h = run(m, in, 1, 200);
    CHECK(h.a == Action::PowerOff);
    in.phys_on = false;
    in.mode = "COOL";
    in.room = 24.4f;  // warmer, but inside delta
    CHECK(run(m, in, h.at + 1, h.at + 400).a == Action::None);
    in.room = 24.5f;
    h = run(m, in, h.at + 400, h.at + 800);
    CHECK(h.a == Action::PowerOn);
    CHECK(m.state() == State::Running);
}

static void test_user_commands() {
    uint32_t now;
    Machine m = running_heat(now);
    uint32_t p = park_it(m, now);
    // Re-asserting ON (HA re-sends "heat") keeps it parked.
    CHECK(m.set_desired_power(true, p + 5) == Action::None);
    CHECK(m.parked());
    // A changed setpoint/mode while parked probes with airflow.
    CHECK(m.notify_intent_change(p + 6) == Action::PowerOn);
    CHECK(m.state() == State::Probing);
    // OFF is a real off, never "parked".
    CHECK(m.set_desired_power(false, p + 7) == Action::PowerOff);
    CHECK(m.state() == State::Off);
    CHECK(!m.managed());
    CHECK(m.notify_intent_change(p + 8) == Action::None);
    CHECK(run(m, heat(15.0f, 21.0f, false), p + 9, p + 3000).a == Action::None);
    CHECK(m.set_desired_power(true, p + 3000) == Action::PowerOn);
    CHECK(m.state() == State::Running);
}

static void test_mode_change_to_fan_wakes() {
    uint32_t now;
    Machine m = running_heat(now);
    uint32_t p = park_it(m, now);
    Inputs fan = heat(21.0f, 21.0f, false);
    fan.mode = "FAN";
    Hit h = run(m, fan, p + 1, p + 5);
    CHECK(h.a == Action::PowerOn);
    CHECK(m.state() == State::Running);
}

static void test_remote_press_while_parked() {
    uint32_t now;
    Machine m = running_heat(now);
    uint32_t p = park_it(m, now);
    // A short blip (driver reverting inside its grace window) is ignored.
    uint32_t t = p + 100;
    CHECK(run(m, heat(21.0f), t, t + 3).a == Action::None);
    CHECK(run(m, heat(21.0f, 21.0f, false), t + 3, t + 20).a == Action::None);
    CHECK(m.parked());
    // A sustained ON is adopted as a probe — no command sent.
    t += 20;
    CHECK(run(m, heat(21.0f), t, t + kExternalHoldS + 1).a == Action::None);
    CHECK(m.state() == State::Probing);
    CHECK(std::string(m.reason()) == "remote");
}

static void test_remote_off_while_running() {
    uint32_t now;
    Machine m = running_heat(now);
    uint32_t t = now + 100;
    CHECK(run(m, heat(19.0f, 21.0f, false), t, t + 30).a == Action::None);
    CHECK(m.state() == State::Off);
    // Remote back on.
    CHECK(run(m, heat(19.0f), t + 30, t + 60).a == Action::None);
    CHECK(m.state() == State::Running);
}

static void test_mismatch_ignored_during_settle() {
    uint32_t now;
    Machine m = running_heat(now);
    uint32_t p = park_it(m, now);
    // Unit still reports ON just after our OFF: inside the settle window.
    CHECK(run(m, heat(21.0f), p + 1, p + kCmdSettleS).a == Action::None);
    CHECK(m.parked());
}

static void test_restore_after_reboot() {
    Machine a;
    a.configure(enabled_cfg(), 0);
    a.restore(true);
    CHECK(a.tick(heat(21.0f, 21.0f, false), 10) == Action::None);
    CHECK(a.parked());
    CHECK(a.next_probe_s(10) == 900);

    Machine b;
    b.configure(enabled_cfg(), 0);
    b.restore(true);
    CHECK(b.tick(heat(21.0f), 10) == Action::None);
    CHECK(b.state() == State::Probing);

    Machine c;  // feature disabled since: desired was ON -> power on
    c.restore(true);
    CHECK(c.tick(heat(21.0f, 21.0f, false), 10) == Action::PowerOn);

    Machine d;  // not managed: plain adopt
    d.configure(enabled_cfg(), 0);
    d.restore(false);
    CHECK(d.tick(heat(21.0f, 21.0f, false), 10) == Action::None);
    CHECK(d.state() == State::Off);
}

static void test_disable_while_parked() {
    uint32_t now;
    Machine m = running_heat(now);
    uint32_t p = park_it(m, now);
    m.take_event();
    Config off;
    CHECK(m.configure(off, p + 10) == Action::PowerOn);
    CHECK(!m.parked());
    CHECK(m.take_event() != nullptr);
    CHECK(run(m, heat(25.0f), p + 11, p + 500).a == Action::None);
}

static void test_disconnected_pauses() {
    uint32_t now;
    Machine m = running_heat(now);
    Inputs in = heat(21.0f);
    in.connected = false;
    CHECK(run(m, in, now, now + 500).a == Action::None);
    CHECK(m.state() == State::Running);
}

// ── Time-slice hold (#133) ──────────────────────────────────────────────────

static void test_shared_config_roundtrip() {
    Config c;
    c.enabled = true;
    c.delta_c = 1.5f;
    c.min_off_s = 600;
    c.probe_enabled = false;
    c.probe_interval_s = 1800;
    c.probe_duration_s = 180;
    CHECK(encode_shared(c) == "1,600,0,1800,180");
    Config local;               // a head's own config: delta stays its own
    local.delta_c = 2.0f;
    CHECK(decode_shared("1,600,0,1800,180", local));
    CHECK(local.enabled && local.min_off_s == 600 && !local.probe_enabled);
    CHECK(local.probe_interval_s == 1800 && local.probe_duration_s == 180);
    CHECK(local.delta_c == 2.0f);
    CHECK(decode_shared("1,10,1,20,5", local));   // sanitized
    CHECK(local.min_off_s == kMinOffFloorS && local.probe_interval_s == kMinOffFloorS);
    CHECK(local.probe_duration_s == kMinProbeDurationS);
    Config keep = local;
    CHECK(!decode_shared("", keep));
    CHECK(!decode_shared("1,600,0,1800", keep));
    CHECK(!decode_shared("2,600,0,1800,180", keep));
    CHECK(!decode_shared("1,600,0,1800,180,", keep));
    CHECK(!decode_shared("1,600,x,1800,180", keep));
    CHECK(keep.min_off_s == local.min_off_s);
}

static void test_hold_parks_running_head() {
    uint32_t now;
    Machine m = running_heat(now);
    CHECK(m.set_hold(true, now) == Action::PowerOff);
    CHECK(m.state() == State::Parked);
    CHECK(std::string(m.reason()) == kWaitingForSlice);
    CHECK(m.parked());
    CHECK(!m.parked_satisfied());
    CHECK(m.take_event() == nullptr);  // slice changes are logged by the coordinator
    CHECK(m.next_probe_s(now) == -1);
}

static void test_hold_blocks_drift_and_probe() {
    uint32_t now;
    Machine m = running_heat(now);
    m.set_hold(true, now);
    // Room far below setpoint for longer than min-off and probe interval: no wake.
    Inputs cold = heat(15.0f, 21.0f, false);
    CHECK(run(m, cold, now + 1, now + 3000).a == Action::None);
    CHECK(m.state() == State::Parked);
}

static void test_hold_survives_ineligible_mode() {
    uint32_t now;
    Machine m = running_heat(now);
    m.set_hold(true, now);
    Inputs in = heat(19.0f, 21.0f, false);
    in.mode = "AUTO";
    CHECK(run(m, in, now + 1, now + 100).a == Action::None);
    CHECK(m.state() == State::Parked);
    CHECK(m.set_hold(false, now + 100) == Action::PowerOn);
    CHECK(m.state() == State::Running);
    CHECK(run(m, in, now + 101, now + 300).a == Action::None);  // AUTO never parks
}

static void test_release_resumes() {
    uint32_t now;
    Machine m = running_heat(now);
    m.set_hold(true, now);
    run(m, heat(19.0f, 21.0f, false), now + 1, now + 100);
    CHECK(m.set_hold(false, now + 100) == Action::PowerOn);
    CHECK(m.state() == State::Running);
    CHECK(std::string(m.reason()).empty());
    CHECK(m.set_hold(false, now + 101) == Action::None);  // idempotent
}

static void test_hold_on_satisfied_head_relabels() {
    uint32_t now;
    Machine m = running_heat(now);
    uint32_t at = park_it(m, now);
    CHECK(m.parked_satisfied());
    CHECK(m.set_hold(true, at + 5) == Action::None);  // already off
    CHECK(std::string(m.reason()) == kWaitingForSlice);
    CHECK(!m.parked_satisfied());
}

static void test_remote_on_while_held_reparks() {
    uint32_t now;
    Machine m = running_heat(now);
    m.set_hold(true, now);
    // IR remote turns the unit on; after the settle + hold windows it is re-parked.
    Hit h = run(m, heat(19.0f), now + 1, now + 200);
    CHECK(h.a == Action::PowerOff);
    CHECK(m.state() == State::Parked);
    CHECK(std::string(m.reason()) == kWaitingForSlice);
}

static void test_turn_on_while_held_stays_off() {
    uint32_t now;
    Machine m = running_heat(now);
    m.set_desired_power(false, now);
    run(m, heat(19.0f, 21.0f, false), now + 1, now + 20);
    m.set_hold(true, now + 20);
    CHECK(m.state() == State::Off);
    CHECK(m.set_desired_power(true, now + 21) == Action::None);  // unit already off
    CHECK(m.state() == State::Parked);
    CHECK(std::string(m.reason()) == kWaitingForSlice);
    CHECK(m.notify_intent_change(now + 22) == Action::None);   // no probe while held
    CHECK(m.set_desired_power(false, now + 23) == Action::PowerOff);
    CHECK(m.state() == State::Off);
}

static void test_hold_applied_on_first_tick() {
    Machine m;
    m.configure(enabled_cfg(), 0);
    m.set_hold(true, 0);
    CHECK(m.tick(heat(19.0f), 0) == Action::PowerOff);  // unit was on
    CHECK(m.state() == State::Parked);
    Machine r;
    r.configure(enabled_cfg(), 0);
    r.restore(true);
    r.set_hold(true, 0);
    CHECK(r.tick(heat(19.0f, 21.0f, false), 0) == Action::None);
    CHECK(r.state() == State::Parked);
    CHECK(std::string(r.reason()) == kWaitingForSlice);
}

static void test_disable_while_held_resumes() {
    uint32_t now;
    Machine m = running_heat(now);
    m.set_hold(true, now);
    Config off;
    CHECK(m.configure(off, now + 5) == Action::PowerOn);
}

int main() {
    test_sanitize();
    test_disabled_is_passthrough();
    test_parks_after_debounce();
    test_no_park_when_not_satisfied();
    test_defrost_resets_debounce();
    test_wake_respects_min_off();
    test_no_wake_inside_delta();
    test_probe_cycle();
    test_probe_finds_room_cold();
    test_probe_cap_without_airflow();
    test_cool_and_dry();
    test_user_commands();
    test_mode_change_to_fan_wakes();
    test_remote_press_while_parked();
    test_remote_off_while_running();
    test_mismatch_ignored_during_settle();
    test_restore_after_reboot();
    test_disable_while_parked();
    test_disconnected_pauses();
    test_shared_config_roundtrip();
    test_hold_parks_running_head();
    test_hold_blocks_drift_and_probe();
    test_hold_survives_ineligible_mode();
    test_release_resumes();
    test_hold_on_satisfied_head_relabels();
    test_remote_on_while_held_reparks();
    test_turn_on_while_held_stays_off();
    test_hold_applied_on_first_tick();
    test_disable_while_held_resumes();
    if (g_failures) {
        std::printf("%d park_logic test(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("All park_logic tests passed\n");
    return 0;
}
