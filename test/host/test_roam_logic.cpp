// Host-side unit tests for the Wi-Fi roaming policy (roam_logic.cpp).
//
// Build & run (see .github/workflows/host-tests.yml):
//   g++ -std=c++17 -Wall -Wextra -I components/roam/include
//       components/roam/roam_logic.cpp test/host/test_roam_logic.cpp

#include "roam_logic.h"

#include <cstdio>
#include <vector>

static int g_failures = 0;

#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);       \
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

using namespace roam;

namespace {

const Bssid kHome   = {0x7a, 0x45, 0x58, 0xa6, 0x39, 0x66};
const Bssid kOffice = {0x7a, 0x45, 0x58, 0xaa, 0x57, 0x43};
const Bssid kOther  = {0xd0, 0x21, 0xf9, 0x4c, 0x44, 0xac};

// Feed 5-second samples on @p b at @p rssi from @p t for @p secs, ticking each
// step; returns the first non-None action (and stops there), else None.
Action run(Policy& p, int64_t& t, const Bssid& b, int rssi, int64_t secs, int ch = 6) {
    for (int64_t end = t + secs; t < end; t += 5) {
        p.sample(t, b, rssi, ch);
        Action a = p.tick(t);
        if (a.act != Act::None) return a;
    }
    return {};
}

bool has(const std::vector<Report>& rs, Report::Kind k) {
    for (const auto& r : rs) if (r.kind == k) return true;
    return false;
}

// A single momentary dip below the threshold never triggers a scan.
void test_momentary_dip_does_not_scan() {
    Policy p;
    int64_t t = 100;
    CHECK(run(p, t, kHome, -63, 120).act == Act::None);
    p.sample(t, kHome, -71, 6);
    CHECK(p.tick(t).act == Act::None);
    t += 5;
    CHECK(run(p, t, kHome, -63, 120).act == Act::None);
}

// Sustained weak link → scan; a candidate that only beats the driver RSSI (but
// not the current AP's own scan RSSI by gain_db) is rejected — the 18:43 bad roam.
void test_rejects_candidate_not_better_than_current_scan() {
    Policy p;
    int64_t t = 100;
    Action a = run(p, t, kHome, -71, 120);
    CHECK(a.act == Act::Scan);
    p.scan_result(t, {{kHome, -63, 6}, {kOffice, -64, 6}});
    CHECK(p.take_reports().empty());
    // Backed off: no new scan for scan_backoff_s.
    t += 5;
    CHECK(run(p, t, kHome, -71, 590).act == Act::None);
    CHECK(run(p, t, kHome, -71, 30).act == Act::Scan);
}

// A scan that doesn't hear the current AP is inconclusive.
void test_missing_current_ap_is_inconclusive() {
    Policy p;
    int64_t t = 100;
    CHECK(run(p, t, kHome, -75, 120).act == Act::Scan);
    p.scan_result(t, {{kOffice, -55, 6}});
    t += 5;
    CHECK(run(p, t, kHome, -75, 300).act == Act::None);
}

// Good candidate must be confirmed by a second scan, then we roam; a candidate
// that fades in the second scan aborts.
void test_confirmation_scan() {
    Policy p;
    int64_t t = 100;
    CHECK(run(p, t, kHome, -78, 120).act == Act::Scan);
    p.scan_result(t, {{kHome, -77, 6}, {kOffice, -60, 11}});
    t += 5;
    CHECK(run(p, t, kHome, -78, 30).act == Act::Scan);  // confirm scan after ~20 s
    p.scan_result(t, {{kHome, -77, 6}, {kOffice, -72, 11}});  // faded
    t += 5;
    CHECK(run(p, t, kHome, -78, 300).act == Act::None);

    Policy q;
    t = 100;
    CHECK(run(q, t, kHome, -78, 120).act == Act::Scan);
    q.scan_result(t, {{kHome, -77, 6}, {kOffice, -60, 11}});
    t += 5;
    CHECK(run(q, t, kHome, -78, 30).act == Act::Scan);
    q.scan_result(t, {{kHome, -76, 6}, {kOffice, -62, 11}});
    Action a = q.tick(t);
    CHECK(a.act == Act::Roam);
    CHECK(a.target == kOffice);
    CHECK(a.channel == 11);
    auto rs = q.take_reports();
    CHECK(rs.size() == 1 && rs[0].kind == Report::Kind::Roaming);
    CHECK(rs[0].from_rssi == -76 && rs[0].to_rssi == -62);  // stronger current, weaker candidate
}

// Helper: drive q to the point where it has just roamed to kOffice.
Policy roamed_policy(int64_t& t) {
    Policy q;
    run(q, t, kHome, -78, 120);
    q.scan_result(t, {{kHome, -77, 6}, {kOffice, -60, 11}});
    t += 5;
    run(q, t, kHome, -78, 30);
    q.scan_result(t, {{kHome, -77, 6}, {kOffice, -62, 11}});
    q.tick(t);
    q.take_reports();
    q.disconnected(t + 1);
    t += 5;
    return q;
}

// After a roam the new link is verified; better → kept.
void test_roam_kept() {
    int64_t t = 100;
    Policy q = roamed_policy(t);
    CHECK(run(q, t, kOffice, -65, 90, 11).act == Act::None);
    auto rs = q.take_reports();
    CHECK(has(rs, Report::Kind::Roamed));
    CHECK(has(rs, Report::Kind::Kept));
    CHECK(!q.blacklisted(kOffice, t));
}

// After a roam the new link averages worse → roam back and blacklist it.
void test_roam_back_and_blacklist() {
    int64_t t = 100;
    Policy q = roamed_policy(t);
    Action a = run(q, t, kOffice, -80, 90, 11);
    CHECK(a.act == Act::Roam);
    CHECK(a.target == kHome);
    CHECK(a.channel == 6);
    CHECK(q.blacklisted(kOffice, t));
    CHECK(has(q.take_reports(), Report::Kind::RoamBack));
    t += 5;
    CHECK(run(q, t, kHome, -78, 30).act == Act::None);
    CHECK(has(q.take_reports(), Report::Kind::RolledBack));
    // Still weak, but the blacklisted AP is never chosen again within the hour.
    CHECK(run(q, t, kHome, -78, 700).act == Act::Scan);
    q.scan_result(t, {{kHome, -77, 6}, {kOffice, -50, 11}});
    t += 5;
    CHECK(run(q, t, kHome, -78, 60).act == Act::None);
    // …and is allowed again once the blacklist expires.
    CHECK(!q.blacklisted(kOffice, t + 3600));
}

// A roam that never lands on the target times out → Abort + blacklist.
void test_roam_timeout() {
    Policy p;
    int64_t t = 100;
    run(p, t, kHome, -78, 120);
    p.scan_result(t, {{kHome, -77, 6}, {kOffice, -60, 11}});
    t += 5;
    run(p, t, kHome, -78, 30);
    p.scan_result(t, {{kHome, -77, 6}, {kOffice, -60, 11}});
    CHECK(p.tick(t).act == Act::Roam);
    t += 5;
    Action a = run(p, t, kHome, -78, 40);  // recovery reconnected to the old AP
    CHECK(a.act == Act::Abort);
    CHECK(p.blacklisted(kOffice, t));
    CHECK(has(p.take_reports(), Report::Kind::RoamFailed));
}

// A move we didn't start (AP steering / reconnect) onto a worse AP is rolled
// back, but only once per cooldown so we never fight the AP in a loop.
void test_unsolicited_move_rollback_cooldown() {
    Policy p;
    int64_t t = 100;
    run(p, t, kHome, -62, 120);
    Action a = run(p, t, kOther, -75, 90, 1);
    CHECK(a.act == Act::Roam && a.target == kHome);
    auto rs = p.take_reports();
    CHECK(has(rs, Report::Kind::Moved) && has(rs, Report::Kind::RoamBack));
    t += 5;
    run(p, t, kHome, -62, 120);
    // Steered away again soon after: no second roll-back within the cooldown.
    a = run(p, t, kOffice, -75, 120, 11);
    CHECK(a.act == Act::None);
    CHECK(has(p.take_reports(), Report::Kind::Moved));
}

// Boot / no history: a BSSID change without a fresh pre-change average is not
// verified (nothing to compare against).
void test_change_without_history_not_verified() {
    Policy p;
    int64_t t = 100;
    p.sample(t, kHome, -62, 6);
    t += 300;  // long outage
    CHECK(run(p, t, kOther, -65, 120, 1).act == Act::None);
    auto rs = p.take_reports();
    CHECK(has(rs, Report::Kind::Moved) && !has(rs, Report::Kind::Kept));
}

// Backoff doubles while nothing better exists and resets when the link recovers.
void test_backoff_doubles_and_resets() {
    Policy p;
    int64_t t = 100;
    CHECK(run(p, t, kHome, -72, 120).act == Act::Scan);
    p.scan_result(t, {{kHome, -70, 6}});
    t += 5;
    CHECK(run(p, t, kHome, -72, 610).act == Act::Scan);  // 600 s
    p.scan_result(t, {{kHome, -70, 6}});
    t += 5;
    CHECK(run(p, t, kHome, -72, 1100).act == Act::None);  // now 1200 s
    CHECK(run(p, t, kHome, -72, 120).act == Act::Scan);
    p.scan_result(t, {{kHome, -70, 6}});
    t += 5;
    run(p, t, kHome, -60, 2500);  // healthy for a while: backoff back to 600
    CHECK(run(p, t, kHome, -72, 120).act == Act::Scan);
    p.scan_result(t, {{kHome, -70, 6}});
    t += 5;
    CHECK(run(p, t, kHome, -72, 620).act == Act::Scan);
}

// An AP-steered move between the confirming scan and the queued roam going out
// drops the stale roam (treated as an unsolicited move instead).
void test_move_cancels_pending_roam() {
    Policy p;
    int64_t t = 100;
    run(p, t, kHome, -78, 120);
    p.scan_result(t, {{kHome, -77, 6}, {kOffice, -60, 11}});
    t += 5;
    run(p, t, kHome, -78, 30);
    p.scan_result(t, {{kHome, -77, 6}, {kOffice, -60, 11}});
    p.take_reports();
    t += 1;
    p.sample(t, kOther, -60, 1);  // BTM to a third AP before tick()
    CHECK(p.tick(t).act == Act::None);
    auto rs = p.take_reports();
    CHECK(has(rs, Report::Kind::Moved));
    CHECK(!has(rs, Report::Kind::RoamFailed));
    CHECK(!p.blacklisted(kOffice, t));
}

// A roam that lands on a third AP is reported failed and the AP we ended up on
// is still verified against the old link (worse → roam back).
void test_wrong_landing_is_verified() {
    Policy p;
    int64_t t = 100;
    run(p, t, kHome, -72, 120);
    p.scan_result(t, {{kHome, -62, 6}, {kOffice, -50, 11}});
    t += 5;
    run(p, t, kHome, -72, 30);
    p.scan_result(t, {{kHome, -62, 6}, {kOffice, -50, 11}});
    CHECK(p.tick(t).act == Act::Roam);
    p.take_reports();
    t += 5;
    Action a = run(p, t, kOther, -85, 90, 1);
    CHECK(has(p.take_reports(), Report::Kind::RoamFailed));
    CHECK(p.blacklisted(kOffice, t));
    CHECK(a.act == Act::Roam && a.target == kHome && a.channel == 6);
    CHECK(p.blacklisted(kOther, t));
}

}  // namespace

int main() {
    test_momentary_dip_does_not_scan();
    test_rejects_candidate_not_better_than_current_scan();
    test_missing_current_ap_is_inconclusive();
    test_confirmation_scan();
    test_roam_kept();
    test_roam_back_and_blacklist();
    test_roam_timeout();
    test_unsolicited_move_rollback_cooldown();
    test_change_without_history_not_verified();
    test_backoff_doubles_and_resets();
    test_move_cancels_pending_roam();
    test_wrong_landing_is_verified();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("roam_logic: all tests passed\n");
    return 0;
}
