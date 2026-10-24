/// @file slice_ctl.cpp
/// @brief Firmware glue for group time-slicing (#133). See slice_ctl.h.

#include "slice_ctl.h"
#include "park_ctl.h"
#include "events.h"
#include "group_config.h"

#include <cctype>
#include <map>
#include <mutex>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

namespace slice_ctl {
namespace {

const char* TAG = "slice";
constexpr const char* kNs = "slice";
constexpr int64_t kPeerFreshUs = 45LL * 1000 * 1000;  // matches the group poller

struct PeerEntry {
    PeerObs obs;
    int64_t updated_us = 0;
};

std::mutex   s_mtx;
std::mutex   s_tick_mtx;       // serializes tick() and Command (CN105 task, httpd, MQTT)
Hooks        s_hooks;
slice::Node  s_node;
std::string  s_cfg_enc;          // last applied encoded config
std::map<std::string, PeerEntry> s_peers;
uint64_t     s_saved_term = 0;
bool         s_configured = false;  // tick() has applied the group's config

// A command recorded but not yet reflected in the unit's reported settings.
// tick() uses it in place of the reported mode so it doesn't flap the hold.
struct Intent {
    bool        active = false;
    bool        power_on = false;
    std::string mode;
    uint32_t    until = 0;
};
Intent       s_pending;
constexpr uint32_t kPendingS = 15;

uint32_t now_s() { return static_cast<uint32_t>(esp_timer_get_time() / 1000000); }

std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

void save_term(uint64_t t) {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u64(h, "term", t);
    nvs_commit(h);
    nvs_close(h);
}

// A held air check is running on this head.
bool self_checking(const park_ctl::Snapshot& ps) {
    return ps.held && ps.state == park::State::Probing;
}

bool is_auto(bool power_on, const std::string& mode) { return power_on && mode == "AUTO"; }

slice::Need need_of(park::Need n) {
    switch (n) {
        case park::Need::No:       return slice::Need::No;
        case park::Need::Checking: return slice::Need::Checking;
        default:                   return slice::Need::Yes;
    }
}

// Directive -> hold, for self. AUTO is held while slicing (it would choose its
// own direction) until auto_ctl turns it into HeatLink's managed Auto.
bool hold_for_locked(bool power_on, const std::string& mode, uint32_t t) {
    if (!s_node.enabled()) return false;
    if (is_auto(power_on, mode)) return true;
    return s_node.directive(slice::desired_of(power_on, mode), t) == slice::Directive::Hold;
}

// Self first, then each enrolled peer (stale peers are present but not fresh).
std::vector<slice::Member> members_locked(bool power_on, const std::string& mode,
                                          const cn105::Status& st, bool satisfied,
                                          slice::Need need,
                                          const std::vector<std::string>& peers) {
    std::vector<slice::Member> ms;
    slice::Member self;
    self.uid = hvac_group::self_uid();
    self.fresh = true;
    self.capable = true;
    self.wants = slice::desired_of(power_on, mode);
    self.satisfied = satisfied;
    self.need = need;
    self.defrost = st.subMode == "DEFROST";
    ms.push_back(self);
    const int64_t now_us = esp_timer_get_time();
    for (const auto& uid : peers) {
        slice::Member m;
        m.uid = uid;
        auto it = s_peers.find(uid);
        if (it != s_peers.end() && now_us - it->second.updated_us < kPeerFreshUs) {
            const PeerObs& o = it->second.obs;
            m.fresh = true;
            m.capable = o.capable;
            m.wants = slice::desired_of(o.power == "ON", upper(o.mode));
            m.satisfied = o.satisfied;
            m.need = o.need;
            m.defrost = o.sub_mode == "DEFROST";
            m.has_state = o.has_state;
            m.state = o.state;
        }
        ms.push_back(m);
    }
    return ms;
}

}  // namespace

void init(const Hooks& hooks) {
    s_hooks = hooks;
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READONLY, &h) == ESP_OK) {
        uint64_t t = 0;
        if (nvs_get_u64(h, "term", &t) == ESP_OK) s_saved_term = t;
        nvs_close(h);
    }
    std::lock_guard<std::mutex> lk(s_mtx);
    s_node.restore_term(s_saved_term);
}

// Self's effective desire: a pending command until the unit reports it.
void self_desire_locked(const cn105::Settings& s, uint32_t t, bool& power_on, std::string& mode) {
    power_on = s.power == "ON";
    mode = upper(s.mode);
    if (!s_pending.active) return;
    const bool reached = power_on == s_pending.power_on &&
                         (!s_pending.power_on || mode == s_pending.mode);
    if (reached || t >= s_pending.until) {
        s_pending.active = false;
        return;
    }
    power_on = s_pending.power_on;
    if (!s_pending.mode.empty()) mode = s_pending.mode;
}

void tick() {
    if (!s_hooks.settings || !s_hooks.status) return;
    std::lock_guard<std::mutex> tick_lk(s_tick_mtx);
    const cn105::Settings s = s_hooks.settings();
    const cn105::Status st = s_hooks.status();
    const park_ctl::Snapshot ps = park_ctl::snapshot();
    const bool grouped = hvac_group::in_group();
    const std::string enc = grouped ? hvac_group::slice_config() : "";
    const std::vector<std::string> peers = grouped ? hvac_group::get().peers
                                                   : std::vector<std::string>{};
    const uint32_t t = now_s();
    // Waiting heads start their air checks this long before a turn decision.
    const uint32_t lead = park_ctl::config().probe_duration_s + 60;

    std::vector<const char*> evs;
    bool enabled = false, hold = false, check = false;
    uint64_t term = 0;
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        if (enc != s_cfg_enc) {
            slice::Config c;
            if (!slice::decode_config(enc, c)) c = slice::Config{};
            s_node.configure(c, t);
            s_cfg_enc = enc;
            ESP_LOGI(TAG, "time-slicing %s (min %us max %us settle %us)",
                     c.enabled ? "enabled" : "disabled", (unsigned)c.min_slice_s,
                     (unsigned)c.max_slice_s, (unsigned)c.settle_s);
        }
        s_configured = true;
        for (auto it = s_peers.begin(); it != s_peers.end();) {
            bool member = false;
            for (const auto& p : peers) member = member || p == it->first;
            it = member ? std::next(it) : s_peers.erase(it);
        }
        bool power_on;
        std::string mode;
        self_desire_locked(s, t, power_on, mode);
        s_node.set_self(hvac_group::self_uid());
        s_node.set_check_lead(lead);
        std::vector<slice::Member> ms =
            members_locked(power_on, mode, st, ps.satisfied, need_of(ps.need), peers);
        s_node.tick(ms, t);
        while (const char* e = s_node.take_event()) evs.push_back(e);
        enabled = s_node.enabled();
        hold = hold_for_locked(power_on, mode, t);
        check = s_node.have_state() && s_node.state().phase != slice::Phase::Changeover &&
                s_node.state().check;
        term = s_node.max_term();
    }
    if (term > s_saved_term) {
        save_term(term);
        s_saved_term = term;
    }
    for (const char* e : evs) {
        ESP_LOGI(TAG, "%s", e);
        events::log(events::Cat::Group, events::Actor::System, e);
    }
    park_ctl::set_slice(enabled, hold, check);
}

Command::Command(bool power_on, const std::string& mode) {
    s_tick_mtx.lock();
    const uint32_t t = now_s();
    bool enabled, hold;
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        s_pending = Intent{true, power_on, upper(mode), t + kPendingS};
        enabled = s_node.enabled();
        hold = hold_for_locked(power_on, s_pending.mode, t);
    }
    // Only ever tighten here: a release must wait until the new mode is recorded.
    if (enabled && hold) park_ctl::set_slice(true, true);
}

Command::~Command() {
    const uint32_t t = now_s();
    bool enabled, hold;
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        enabled = s_node.enabled();
        hold = hold_for_locked(s_pending.power_on, s_pending.mode, t);
    }
    park_ctl::set_slice(enabled, hold);
    s_tick_mtx.unlock();
}

bool enabled() {
    std::lock_guard<std::mutex> lk(s_mtx);
    return s_node.enabled();
}

bool configured() {
    std::lock_guard<std::mutex> lk(s_mtx);
    return s_configured;
}

void observe_peer(const std::string& uid, const PeerObs& obs) {
    std::lock_guard<std::mutex> lk(s_mtx);
    s_peers[uid] = PeerEntry{obs, esp_timer_get_time()};
}

Publish publish() {
    const park_ctl::Snapshot ps = park_ctl::snapshot();
    std::lock_guard<std::mutex> lk(s_mtx);
    Publish p;
    p.has_state = s_node.have_state();
    p.state = s_node.state();
    p.state.elapsed_s = s_node.elapsed_now(now_s());
    p.satisfied = ps.satisfied;
    p.need = need_of(ps.need);
    p.checking = self_checking(ps);
    return p;
}

Status status() {
    const cn105::Settings s = s_hooks.settings ? s_hooks.settings() : cn105::Settings{};
    const cn105::Status st = s_hooks.status ? s_hooks.status() : cn105::Status{};
    const park_ctl::Snapshot ps = park_ctl::snapshot();
    Status out;
    out.in_group = hvac_group::in_group();
    const std::vector<std::string> peers = out.in_group ? hvac_group::get().peers
                                                        : std::vector<std::string>{};
    const uint32_t t = now_s();
    std::lock_guard<std::mutex> lk(s_mtx);
    out.cfg = s_node.config();
    out.have_state = s_node.have_state();
    out.is_coordinator = s_node.is_coordinator();
    out.coordinator = s_node.coordinator_uid();
    out.state = s_node.state();
    out.elapsed_s = s_node.elapsed_now(t);
    out.all_capable = out.in_group;
    bool power_on;
    std::string mode;
    self_desire_locked(s, t, power_on, mode);
    for (const auto& m : members_locked(power_on, mode, st, ps.satisfied, need_of(ps.need), peers)) {
        MemberStatus ms;
        ms.uid = m.uid;
        ms.fresh = m.fresh;
        ms.capable = m.capable;
        ms.wants = m.wants;
        ms.need = m.need;
        ms.satisfied = m.satisfied;
        ms.directive = s_node.directive(m.wants, t);
        if (out.members.empty()) {
            ms.checking = self_checking(ps);
        } else {
            auto it = s_peers.find(m.uid);
            ms.checking = m.fresh && it != s_peers.end() && it->second.obs.checking;
        }
        if (!m.fresh || !m.capable) out.all_capable = false;
        out.members.push_back(ms);
    }
    return out;
}

}  // namespace slice_ctl
