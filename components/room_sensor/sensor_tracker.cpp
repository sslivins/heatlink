/// @file sensor_tracker.cpp
/// @brief Room-sensor pairing + freshness (see sensor_tracker.h). No ESP-IDF deps.

#include "sensor_tracker.h"

#include <algorithm>

namespace room_sensor {

void Tracker::set_paired(const std::string& addr, const std::string& name) {
    if (addr != addr_) sample_ = Sample{};
    addr_ = addr;
    name_ = addr.empty() ? std::string() : name;
    pairing_ = false;
}

void Tracker::start_pairing(uint32_t now_s) {
    pairing_ = true;
    pair_start_ = now_s;
    nearby_.clear();
}

uint32_t Tracker::pairing_remaining_s(uint32_t now_s) const {
    if (!pairing(now_s)) return 0;
    return kPairWindowS - (now_s - pair_start_);
}

bool Tracker::select(const std::string& addr, uint32_t now_s) {
    for (const Nearby& n : nearby_) {
        if (n.addr != addr) continue;
        if (n.encrypted || now_s - n.seen_at_s >= kNearbyTtlS) return false;
        set_paired(n.addr, n.name);
        if (n.has_temp) {
            sample_.valid = true;
            sample_.temp_c = n.temp_c;
            sample_.temp_at_s = n.seen_at_s;
            sample_.rssi = n.rssi;
            sample_.seen = true;
            sample_.seen_at_s = n.seen_at_s;
        }
        return true;
    }
    return false;
}

void Tracker::note_nearby(const std::string& addr, int rssi, const bthome::Reading& r,
                          uint32_t now_s) {
    auto it = std::find_if(nearby_.begin(), nearby_.end(),
                           [&](const Nearby& n) { return n.addr == addr; });
    if (it == nearby_.end()) {
        // Drop the stalest entry when full.
        if (nearby_.size() >= kMaxNearby) {
            auto old = std::min_element(nearby_.begin(), nearby_.end(),
                                        [](const Nearby& a, const Nearby& b) {
                                            return a.seen_at_s < b.seen_at_s;
                                        });
            nearby_.erase(old);
        }
        nearby_.push_back(Nearby{});
        it = nearby_.end() - 1;
        it->addr = addr;
    }
    it->rssi = rssi;
    it->encrypted = r.encrypted;
    it->seen_at_s = now_s;
    if (!r.name.empty()) it->name = r.name;
    if (r.has_temp) {
        it->has_temp = true;
        it->temp_c = r.temp_c;
    }
}

Tracker::Result Tracker::on_advert(const std::string& addr, int rssi,
                                   const bthome::Reading& r, uint32_t now_s) {
    const bool in_window = pairing(now_s);
    if (in_window) note_nearby(addr, rssi, r, now_s);
    if (in_window && !r.encrypted && r.has_button && r.button != bthome::kButtonNone) {
        std::string name;
        for (const Nearby& n : nearby_)
            if (n.addr == addr) name = n.name;
        set_paired(addr, name);
        // Fall through so this advert's own reading (if any) is kept.
        if (r.has_temp || r.has_humidity || r.has_battery) on_advert(addr, rssi, r, now_s);
        sample_.seen = true;
        sample_.seen_at_s = now_s;
        sample_.rssi = rssi;
        return Result::Paired;
    }
    if (addr != addr_ || r.encrypted) return Result::Ignored;
    if (!r.name.empty() && name_.empty()) name_ = r.name;
    sample_.seen = true;
    sample_.seen_at_s = now_s;
    sample_.rssi = rssi;
    if (r.has_temp) {
        sample_.valid = true;
        sample_.temp_c = r.temp_c;
        sample_.temp_at_s = now_s;
    }
    if (r.has_humidity) {
        sample_.has_humidity = true;
        sample_.humidity = r.humidity;
    }
    if (r.has_battery) {
        sample_.has_battery = true;
        sample_.battery = r.battery;
    }
    return Result::Updated;
}

std::vector<Nearby> Tracker::nearby(uint32_t now_s) const {
    std::vector<Nearby> out;
    for (const Nearby& n : nearby_)
        if (now_s - n.seen_at_s < kNearbyTtlS) out.push_back(n);
    std::sort(out.begin(), out.end(),
              [](const Nearby& a, const Nearby& b) { return a.rssi > b.rssi; });
    return out;
}

}  // namespace room_sensor
