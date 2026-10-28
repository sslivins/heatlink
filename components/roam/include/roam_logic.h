/// @file roam_logic.h
/// @brief Station-initiated Wi-Fi roaming policy — pure, host-testable.
///
/// The IDF roaming app's low-RSSI trigger roams on a single momentary RSSI dip
/// to any AP that is merely 1 dB better in one scan, comparing the scanned
/// candidate against the driver's link RSSI (which on these heads reads several
/// dB below the same AP's beacon RSSI). That combination let a head leave its
/// best AP for a worse one. This policy only roams once it is convinced:
///
///  1. Trigger: the link RSSI *averaged over a window* is below the threshold.
///  2. Candidate: in one scan, another AP beats the current AP's RSSI from the
///     same scan (and the link average) by at least @c gain_db; a second scan
///     a little later must confirm the same candidate before switching.
///  3. Verify: after any BSSID change, the new link is averaged; if it is worse
///     than the old one, roam back and avoid the new AP for @c blacklist_s.
///
/// No ESP-IDF dependencies: main/roam_ctl.cpp feeds it link samples, scan
/// results and a monotonic clock, and executes the Actions it returns.

#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <vector>

namespace roam {

using Bssid = std::array<uint8_t, 6>;

struct Config {
    int     threshold_dbm       = -70;   ///< roam trigger: window average below this
    int64_t window_s            = 60;    ///< link-average window
    int     min_samples         = 9;     ///< samples needed for a valid average
    int     gain_db             = 8;     ///< candidate must beat current by this
    int64_t confirm_delay_s     = 20;    ///< gap between the two scans
    int64_t scan_backoff_s      = 600;   ///< wait after a scan that found nothing…
    int64_t scan_backoff_max_s  = 3600;  ///< …doubling up to this
    int     recover_hyst_db     = 3;     ///< average this far above threshold resets backoff
    int64_t roam_timeout_s      = 30;    ///< time allowed to land on the target
    int64_t verify_window_s     = 60;    ///< post-change averaging window
    int64_t blacklist_s         = 3600;  ///< how long a rejected AP is avoided
    int64_t fresh_s             = 120;   ///< pre-change average must be this recent
    int64_t rollback_cooldown_s = 3600;  ///< min gap between roam-backs after moves we didn't start
};

struct ScanAp {
    Bssid bssid{};
    int   rssi    = 0;
    int   channel = 0;
};

enum class Act : uint8_t {
    None,
    Scan,   ///< run one scan for our SSID, then call scan_result()/scan_failed()
    Roam,   ///< directed reconnect to @c target on @c channel
    Abort,  ///< a roam timed out: drop any BSSID pin and reconnect normally
};

struct Action {
    Act   act = Act::None;
    Bssid target{};
    int   channel = 0;
};

/// Something worth recording in the activity log. The glue drains these.
struct Report {
    enum class Kind : uint8_t {
        Roaming,     ///< confirmed candidate; roaming (scan RSSIs: current → candidate)
        Roamed,      ///< landed on the target of our roam
        Moved,       ///< BSSID changed without us (AP steering / reconnect)
        Kept,        ///< post-change verification passed (link averages)
        RoamBack,    ///< new AP averaged worse than the old one; going back
        RolledBack,  ///< landed back on the old AP
        RoamFailed,  ///< didn't reach the target in time; target avoided
    };
    Kind  kind{};
    Bssid from{}, to{};
    int   from_rssi = 0;
    int   to_rssi   = 0;
};

class Policy {
public:
    explicit Policy(const Config& cfg = Config{}) : cfg_(cfg), backoff_s_(cfg.scan_backoff_s) {}

    /// Connected-link sample (call every few seconds while associated).
    void sample(int64_t now, const Bssid& bssid, int rssi, int channel);
    /// The STA is not associated.
    void disconnected(int64_t now);
    /// Results of a scan this policy requested (APs of our SSID). A scan that
    /// did not hear the current AP is treated as inconclusive.
    void scan_result(int64_t now, const std::vector<ScanAp>& aps);
    /// The requested scan could not be run.
    void scan_failed(int64_t now);
    /// Advance timers and return the next thing for the glue to do.
    Action tick(int64_t now);

    bool blacklisted(const Bssid& b, int64_t now) const;
    /// Window average of the current link; false if not enough samples.
    bool link_average(int64_t now, int& avg) const;

    std::vector<Report> take_reports() { std::vector<Report> r; r.swap(reports_); return r; }

private:
    enum class Phase : uint8_t { Monitor, Scan1, Confirm, Scan2, Roaming, Verify };
    struct Sample { int64_t t; int rssi; };

    bool average_of(const std::deque<Sample>& s, int64_t now, int64_t window, int& avg) const;
    /// Best AP in @p aps beating the current one by gain_db (only @p only if
    /// non-null). @p cur_rssi gets the current AP's RSSI in this scan.
    bool pick_candidate(const std::vector<ScanAp>& aps, int64_t now, int link_avg,
                        const Bssid* only, ScanAp& cand, int& cur_rssi) const;
    void no_candidate(int64_t now);
    void begin_verify(int64_t now, const Bssid& from, int from_avg, int from_ch, bool ours);
    void blacklist(const Bssid& b, int64_t now);
    void report(Report::Kind k, const Bssid& from, const Bssid& to, int fr, int tr);

    Config  cfg_;
    Phase   phase_ = Phase::Monitor;
    bool    connected_ = false;
    Bssid   cur_{};
    int     cur_ch_ = 0;
    int64_t last_sample_t_ = -1;
    std::deque<Sample> samples_;  ///< current link, within window_s

    int64_t next_scan_at_ = 0;
    int64_t backoff_s_;
    int64_t phase_until_ = 0;  ///< confirm-at / roam deadline / verify end

    ScanAp cand_{};            ///< first-scan candidate awaiting confirmation
    int    cand_rssi_ = 0;     ///< weaker of its two scan readings
    int    cur_scan_rssi_ = 0; ///< stronger of the current AP's two scan readings

    // Roaming / Verify context.
    Bssid  from_{}, to_{};
    int    from_avg_ = 0;
    int    from_ch_ = 0;
    bool   rollback_ = false;  ///< the Roaming phase is a roam-back
    bool   ours_ = false;      ///< the change being verified was our own roam
    Action pending_{};
    bool    rolled_back_ever_ = false;
    int64_t last_rollback_at_ = 0;

    struct Banned { Bssid b; int64_t until; };
    std::vector<Banned> banned_;
    std::vector<Report> reports_;
};

}  // namespace roam
