/// @file roam_logic.cpp
/// @brief Station-initiated Wi-Fi roaming policy (see roam_logic.h).

#include "roam_logic.h"

#include <algorithm>
#include <cmath>

namespace roam {

bool Policy::average_of(const std::deque<Sample>& s, int64_t now, int64_t window, int& avg) const {
    long sum = 0;
    int n = 0;
    for (const auto& x : s) {
        if (x.t > now - window && x.t <= now) { sum += x.rssi; n++; }
    }
    if (n < cfg_.min_samples) return false;
    avg = static_cast<int>(std::lround(static_cast<double>(sum) / n));
    return true;
}

bool Policy::link_average(int64_t now, int& avg) const {
    return connected_ && average_of(samples_, now, cfg_.window_s, avg);
}

bool Policy::blacklisted(const Bssid& b, int64_t now) const {
    for (const auto& x : banned_) if (x.b == b && now < x.until) return true;
    return false;
}

void Policy::blacklist(const Bssid& b, int64_t now) {
    for (auto& x : banned_) if (x.b == b) { x.until = now + cfg_.blacklist_s; return; }
    banned_.push_back({b, now + cfg_.blacklist_s});
}

void Policy::report(Report::Kind k, const Bssid& from, const Bssid& to, int fr, int tr) {
    Report r;
    r.kind = k; r.from = from; r.to = to; r.from_rssi = fr; r.to_rssi = tr;
    reports_.push_back(r);
}

void Policy::begin_verify(int64_t now, const Bssid& from, int from_avg, int from_ch, bool ours) {
    phase_ = Phase::Verify;
    from_ = from;
    from_avg_ = from_avg;
    from_ch_ = from_ch;
    to_ = cur_;
    ours_ = ours;
    phase_until_ = now + cfg_.verify_window_s;
}

void Policy::sample(int64_t now, const Bssid& bssid, int rssi, int channel) {
    if (last_sample_t_ >= 0 && bssid != cur_) {
        const Bssid old = cur_;
        const int old_ch = cur_ch_;
        int old_avg = 0;
        const bool fresh = now - last_sample_t_ <= cfg_.fresh_s &&
                           average_of(samples_, last_sample_t_, cfg_.window_s, old_avg);
        samples_.clear();
        cur_ = bssid;
        cur_ch_ = channel;

        if (pending_.act != Act::None) {
            // Moved (e.g. 802.11v) before our queued roam went out: it's stale.
            pending_ = {};
            phase_ = Phase::Monitor;
        }
        if (phase_ == Phase::Roaming) {
            if (bssid == to_) {
                if (rollback_) {
                    report(Report::Kind::RolledBack, from_, to_, 0, 0);
                    phase_ = Phase::Monitor;
                    next_scan_at_ = now + backoff_s_;
                } else {
                    report(Report::Kind::Roamed, from_, to_, from_avg_, 0);
                    begin_verify(now, from_, from_avg_, from_ch_, true);
                }
            } else {
                // Landed somewhere else: the roam failed. Still check the AP we
                // ended up on against the link we had before the roam.
                if (!rollback_) blacklist(to_, now);
                report(Report::Kind::RoamFailed, from_, to_, 0, 0);
                next_scan_at_ = now + backoff_s_;
                if (rollback_) {
                    begin_verify(now, to_, from_avg_, from_ch_, true);
                } else {
                    begin_verify(now, from_, from_avg_, from_ch_, true);
                }
            }
        } else {
            // Not our doing (AP steering, or a reconnect that picked another AP).
            // Any in-flight scan/confirmation or verification is moot.
            report(Report::Kind::Moved, old, bssid, fresh ? old_avg : 0, 0);
            phase_ = Phase::Monitor;
            if (fresh) begin_verify(now, old, old_avg, old_ch, false);
        }
    } else if (last_sample_t_ < 0) {
        cur_ = bssid;
    }
    cur_ch_ = channel;
    connected_ = true;
    last_sample_t_ = now;
    samples_.push_back({now, rssi});
    const int64_t keep = std::max(cfg_.window_s, cfg_.verify_window_s);
    while (!samples_.empty() && samples_.front().t <= now - keep) samples_.pop_front();
}

void Policy::disconnected(int64_t now) {
    connected_ = false;
    if (pending_.act != Act::None) {
        pending_ = {};
        phase_ = Phase::Monitor;
        next_scan_at_ = now + backoff_s_;
    }
    if (phase_ == Phase::Scan1 || phase_ == Phase::Confirm || phase_ == Phase::Scan2) {
        phase_ = Phase::Monitor;
    }
}

bool Policy::pick_candidate(const std::vector<ScanAp>& aps, int64_t now, int link_avg,
                            const Bssid* only, ScanAp& cand, int& cur_rssi) const {
    bool have_cur = false;
    for (const auto& a : aps) {
        if (a.bssid == cur_ && (!have_cur || a.rssi > cur_rssi)) { cur_rssi = a.rssi; have_cur = true; }
    }
    if (!have_cur) return false;  // didn't hear our own AP: inconclusive
    const int base = std::max(cur_rssi, link_avg);
    bool found = false;
    for (const auto& a : aps) {
        if (a.bssid == cur_ || blacklisted(a.bssid, now)) continue;
        if (only && a.bssid != *only) continue;
        if (a.rssi < base + cfg_.gain_db) continue;
        if (!found || a.rssi > cand.rssi) { cand = a; found = true; }
    }
    return found;
}

void Policy::no_candidate(int64_t now) {
    phase_ = Phase::Monitor;
    next_scan_at_ = now + backoff_s_;
    backoff_s_ = std::min(backoff_s_ * 2, cfg_.scan_backoff_max_s);
}

void Policy::scan_result(int64_t now, const std::vector<ScanAp>& aps) {
    if (phase_ != Phase::Scan1 && phase_ != Phase::Scan2) return;
    int avg = 0;
    if (!link_average(now, avg)) { no_candidate(now); return; }
    ScanAp c;
    int cur_rssi = 0;
    if (phase_ == Phase::Scan1) {
        if (!pick_candidate(aps, now, avg, nullptr, c, cur_rssi)) { no_candidate(now); return; }
        cand_ = c;
        cand_rssi_ = c.rssi;
        cur_scan_rssi_ = cur_rssi;
        phase_ = Phase::Confirm;
        phase_until_ = now + cfg_.confirm_delay_s;
        return;
    }
    // Scan2: the same candidate must still clear the bar.
    if (!pick_candidate(aps, now, avg, &cand_.bssid, c, cur_rssi)) { no_candidate(now); return; }
    cand_rssi_ = std::min(cand_rssi_, c.rssi);
    cur_scan_rssi_ = std::max(cur_scan_rssi_, cur_rssi);
    report(Report::Kind::Roaming, cur_, c.bssid, cur_scan_rssi_, cand_rssi_);
    from_ = cur_;
    from_avg_ = avg;
    from_ch_ = cur_ch_;
    to_ = c.bssid;
    rollback_ = false;
    phase_ = Phase::Roaming;
    phase_until_ = now + cfg_.roam_timeout_s;
    pending_ = {Act::Roam, c.bssid, c.channel};
}

void Policy::scan_failed(int64_t now) {
    if (phase_ != Phase::Scan1 && phase_ != Phase::Scan2) return;
    phase_ = Phase::Monitor;
    next_scan_at_ = now + 60;
}

Action Policy::tick(int64_t now) {
    banned_.erase(std::remove_if(banned_.begin(), banned_.end(),
                                 [now](const Banned& b) { return now >= b.until; }),
                  banned_.end());
    if (pending_.act != Act::None) {
        Action a = pending_;
        pending_ = {};
        return a;
    }
    int avg = 0;
    switch (phase_) {
    case Phase::Monitor:
        if (!link_average(now, avg)) return {};
        if (avg >= cfg_.threshold_dbm + cfg_.recover_hyst_db) backoff_s_ = cfg_.scan_backoff_s;
        if (avg < cfg_.threshold_dbm && now >= next_scan_at_) {
            phase_ = Phase::Scan1;
            return {Act::Scan, {}, 0};
        }
        return {};
    case Phase::Confirm:
        if (now < phase_until_) return {};
        phase_ = Phase::Scan2;
        return {Act::Scan, {}, 0};
    case Phase::Roaming:
        if (now < phase_until_) return {};
        if (!rollback_) blacklist(to_, now);
        report(Report::Kind::RoamFailed, from_, to_, 0, 0);
        phase_ = Phase::Monitor;
        next_scan_at_ = now + backoff_s_;
        return {Act::Abort, to_, 0};
    case Phase::Verify: {
        if (now < phase_until_) return {};
        if (!average_of(samples_, now, cfg_.verify_window_s, avg)) return {};  // wait for samples
        const bool may_rollback = ours_ || !rolled_back_ever_ ||
                                  now - last_rollback_at_ >= cfg_.rollback_cooldown_s;
        if (avg < from_avg_ && may_rollback) {
            blacklist(to_, now);
            report(Report::Kind::RoamBack, to_, from_, avg, from_avg_);
            rolled_back_ever_ = true;
            last_rollback_at_ = now;
            const Bssid back = from_;
            from_ = to_;
            to_ = back;
            rollback_ = true;
            phase_ = Phase::Roaming;
            phase_until_ = now + cfg_.roam_timeout_s;
            return {Act::Roam, back, from_ch_};
        }
        report(Report::Kind::Kept, from_, to_, from_avg_, avg);
        phase_ = Phase::Monitor;
        next_scan_at_ = std::max(next_scan_at_, now + cfg_.scan_backoff_s);
        return {};
    }
    case Phase::Scan1:
    case Phase::Scan2:
        return {};
    }
    return {};
}

}  // namespace roam
