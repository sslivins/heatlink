/// @file sensor_set.cpp
/// @brief A head's wireless room sensors (see sensor_set.h). No ESP-IDF deps.

#include "sensor_set.h"

#include <algorithm>

namespace room_sensor {

const char* kind_str(Kind k) { return k == Kind::ShellyRpc ? "shelly" : "bthome"; }

bool kind_from_str(const std::string& s, Kind& out) {
    if (s == "bthome") out = Kind::BTHome;
    else if (s == "shelly") out = Kind::ShellyRpc;
    else return false;
    return true;
}

uint32_t stale_s(Kind k) { return k == Kind::ShellyRpc ? kWifiStaleS : kBleStaleS; }

Sensor* SensorSet::find(const std::string& id) {
    for (Sensor& s : sensors_)
        if (s.id == id) return &s;
    return nullptr;
}

const Sensor* SensorSet::find(const std::string& id) const {
    for (const Sensor& s : sensors_)
        if (s.id == id) return &s;
    return nullptr;
}

bool SensorSet::add(const Sensor& s) {
    if (s.id.empty()) return false;
    pairing_ = false;
    if (Sensor* e = find(s.id)) {
        *e = s;
        return true;
    }
    if (full()) return false;
    sensors_.push_back(s);
    return true;
}

bool SensorSet::remove(const std::string& id) {
    auto it = std::find_if(sensors_.begin(), sensors_.end(),
                           [&](const Sensor& s) { return s.id == id; });
    if (it == sensors_.end()) return false;
    sensors_.erase(it);
    return true;
}

bool SensorSet::room_temp(uint32_t now_s, float* out, size_t* n_fresh) const {
    float sum = 0.0f;
    size_t n = 0;
    for (const Sensor& s : sensors_) {
        if (!fresh(s, now_s)) continue;
        sum += s.sample.temp_c;
        ++n;
    }
    if (n_fresh) *n_fresh = n;
    if (n == 0) return false;
    if (out) *out = sum / static_cast<float>(n);
    return true;
}

void SensorSet::start_pairing(uint32_t now_s) {
    pairing_ = true;
    pair_start_ = now_s;
    nearby_.clear();
}

uint32_t SensorSet::pairing_remaining_s(uint32_t now_s) const {
    if (!pairing(now_s)) return 0;
    return kPairWindowS - (now_s - pair_start_);
}

void SensorSet::apply(Sample& s, const bthome::Reading& r, int rssi, uint32_t now_s) {
    s.seen = true;
    s.seen_at_s = now_s;
    s.has_rssi = true;
    s.rssi = rssi;
    if (r.has_temp) {
        s.valid = true;
        s.temp_c = r.temp_c;
        s.temp_at_s = now_s;
    }
    if (r.has_humidity) {
        s.has_humidity = true;
        s.humidity = r.humidity;
    }
    if (r.has_battery) {
        s.has_battery = true;
        s.battery = r.battery;
    }
}

SensorSet::Result SensorSet::select(const std::string& addr, uint32_t now_s) {
    for (const Nearby& n : nearby_) {
        if (n.addr != addr) continue;
        if (n.encrypted || now_s - n.seen_at_s >= kNearbyTtlS) return Result::Ignored;
        if (find(addr)) return Result::Ignored;
        if (full()) return Result::Full;
        Sensor s;
        s.id = n.addr;
        s.kind = Kind::BTHome;
        s.name = n.name;
        if (n.has_temp) {
            s.sample.valid = true;
            s.sample.temp_c = n.temp_c;
            s.sample.temp_at_s = n.seen_at_s;
        }
        s.sample.seen = true;
        s.sample.seen_at_s = n.seen_at_s;
        s.sample.has_rssi = true;
        s.sample.rssi = n.rssi;
        add(s);
        return Result::Paired;
    }
    return Result::Ignored;
}

void SensorSet::note_nearby(const std::string& addr, int rssi, const bthome::Reading& r,
                            uint32_t now_s) {
    auto it = std::find_if(nearby_.begin(), nearby_.end(),
                           [&](const Nearby& n) { return n.addr == addr; });
    if (it == nearby_.end()) {
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

SensorSet::Result SensorSet::on_advert(const std::string& addr, int rssi,
                                       const bthome::Reading& r, uint32_t now_s) {
    const bool in_window = pairing(now_s);
    // Never list a sensor that's already paired as a pairing candidate.
    Sensor* s = find(addr);
    if (in_window && !s) note_nearby(addr, rssi, r, now_s);
    if (s) {
        if (s->kind != Kind::BTHome || r.encrypted) return Result::Ignored;
        if (!r.name.empty() && s->name.empty()) s->name = r.name;
        apply(s->sample, r, rssi, now_s);
        return Result::Updated;
    }
    if (in_window && !r.encrypted && r.has_button && r.button != bthome::kButtonNone) {
        if (full()) return Result::Full;
        Sensor n;
        n.id = addr;
        n.kind = Kind::BTHome;
        for (const Nearby& nb : nearby_)
            if (nb.addr == addr) n.name = nb.name;
        apply(n.sample, r, rssi, now_s);
        add(n);
        return Result::Paired;
    }
    return Result::Ignored;
}

std::vector<Nearby> SensorSet::nearby(uint32_t now_s) const {
    std::vector<Nearby> out;
    for (const Nearby& n : nearby_)
        if (now_s - n.seen_at_s < kNearbyTtlS && !find(n.addr)) out.push_back(n);
    std::sort(out.begin(), out.end(),
              [](const Nearby& a, const Nearby& b) { return a.rssi > b.rssi; });
    return out;
}

bool SensorSet::wants_scan(uint32_t now_s) const {
    if (pairing(now_s)) return true;
    for (const Sensor& s : sensors_)
        if (s.kind == Kind::BTHome) return true;
    return false;
}

SensorSet::Result SensorSet::on_report(const shelly::Report& r, uint32_t now_s,
                                       std::string* id_out) {
    for (Sensor& s : sensors_) {
        if (s.kind != Kind::ShellyRpc || !shelly::token_equal(s.token, r.token)) continue;
        Sample& m = s.sample;
        m.seen = true;
        m.seen_at_s = now_s;
        if (r.has_rssi) {
            m.has_rssi = true;
            m.rssi = r.rssi;
        }
        if (r.has_temp) {
            m.valid = true;
            m.temp_c = r.temp_c;
            m.temp_at_s = now_s;
        }
        if (r.has_humidity) {
            m.has_humidity = true;
            m.humidity = r.humidity;
        }
        if (r.has_battery) {
            m.has_battery = true;
            m.battery = static_cast<uint8_t>(r.battery);
        }
        if (id_out) *id_out = s.id;
        return Result::Updated;
    }
    return Result::Ignored;
}

}  // namespace room_sensor
