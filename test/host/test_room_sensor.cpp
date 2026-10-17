// Host-side unit tests for the BTHome parser and room-sensor tracker (#142).
//
// Build & run (see .github/workflows/host-tests.yml):
//   g++ -std=c++17 -Wall -Wextra -I components/room_sensor/include
//       components/room_sensor/bthome.cpp components/room_sensor/sensor_tracker.cpp
//       test/host/test_room_sensor.cpp

#include "bthome.h"
#include "sensor_tracker.h"

#include <cmath>
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

static bool near(float a, float b) { return std::fabs(a - b) < 0.001f; }

using bthome::Reading;
using room_sensor::Tracker;

namespace {

// Flags AD + a BTHome service-data AD built from the given object bytes.
std::vector<uint8_t> adv(const std::vector<uint8_t>& objs, uint8_t info = 0x44,
                         const char* name = nullptr) {
    std::vector<uint8_t> a = {0x02, 0x01, 0x06};
    a.push_back(static_cast<uint8_t>(4 + objs.size()));
    a.push_back(0x16);
    a.push_back(0xD2);
    a.push_back(0xFC);
    a.push_back(info);
    a.insert(a.end(), objs.begin(), objs.end());
    if (name) {
        std::string n(name);
        a.push_back(static_cast<uint8_t>(1 + n.size()));
        a.push_back(0x09);
        a.insert(a.end(), n.begin(), n.end());
    }
    return a;
}

Reading parse(const std::vector<uint8_t>& a, bool* ok = nullptr) {
    Reading r;
    bool f = bthome::parse(a.data(), a.size(), r);
    if (ok) *ok = f;
    return r;
}

// Shelly BLU H&T style: packet id, battery, humidity (u8), button, temp 0.1.
std::vector<uint8_t> blu_ht(float temp, uint8_t hum = 45, uint8_t batt = 100,
                            int button = -1, uint8_t pid = 1) {
    int16_t t = static_cast<int16_t>(std::lround(temp * 10.0f));
    std::vector<uint8_t> o = {0x00, pid, 0x01, batt, 0x2E, hum};
    if (button >= 0) {
        o.push_back(0x3A);
        o.push_back(static_cast<uint8_t>(button));
    }
    o.push_back(0x45);
    o.push_back(static_cast<uint8_t>(t & 0xFF));
    o.push_back(static_cast<uint8_t>((t >> 8) & 0xFF));
    return adv(o);
}

}  // namespace

static void test_parse_blu_ht() {
    bool ok = false;
    Reading r = parse(blu_ht(22.7f, 58, 97, -1, 0x2A), &ok);
    CHECK(ok);
    CHECK(!r.encrypted);
    CHECK(r.trigger);
    CHECK(r.has_packet_id && r.packet_id == 0x2A);
    CHECK(r.has_battery && r.battery == 97);
    CHECK(r.has_humidity && near(r.humidity, 58.0f));
    CHECK(r.has_temp && near(r.temp_c, 22.7f));
    CHECK(!r.has_button);

    r = parse(blu_ht(-5.3f, 40, 80, bthome::kButtonPress));
    CHECK(r.has_temp && near(r.temp_c, -5.3f));
    CHECK(r.has_button && r.button == bthome::kButtonPress);
}

static void test_parse_variants() {
    // 0.01 °C temperature + 0.01 % humidity, non-trigger.
    Reading r = parse(adv({0x02, 0xCA, 0x08, 0x03, 0xBF, 0x13}, 0x40, "SBHT-003C"));
    CHECK(r.has_temp && near(r.temp_c, 22.50f));
    CHECK(r.has_humidity && near(r.humidity, 50.55f));
    CHECK(!r.trigger);
    CHECK(r.name == "SBHT-003C");

    // Variable-length text object is skipped, temp after it still decoded.
    r = parse(adv({0x53, 0x03, 'a', 'b', 'c', 0x57, 0xEC}));
    CHECK(r.has_temp && near(r.temp_c, -20.0f));

    // Unknown object id stops decoding but keeps earlier objects.
    r = parse(adv({0x01, 0x50, 0x30, 0x01, 0x45, 0x10, 0x00}));
    CHECK(r.has_battery && r.battery == 0x50);
    CHECK(!r.has_temp);

    // Truncated object: no read past the end.
    bool ok = false;
    r = parse(adv({0x45, 0x10}), &ok);
    CHECK(ok && !r.has_temp);

    // Encrypted: flagged, nothing decoded.
    r = parse(adv({0x12, 0x34, 0x56, 0x78}, 0x41), &ok);
    CHECK(ok && r.encrypted && !r.has_temp);

    // BTHome v1 / other UUIDs aren't ours.
    parse(adv({0x45, 0x10, 0x00}, 0x20), &ok);
    CHECK(!ok);
    std::vector<uint8_t> other = {0x02, 0x01, 0x06, 0x05, 0x16, 0x1A, 0x18, 0x01, 0x02};
    parse(other, &ok);
    CHECK(!ok);

    // Malformed AD length doesn't overrun.
    std::vector<uint8_t> bad = {0x02, 0x01, 0x06, 0x20, 0x16, 0xD2, 0xFC};
    parse(bad, &ok);
    CHECK(!ok);
}

static void test_addr() {
    const uint8_t le[6] = {0x3C, 0x00, 0x12, 0x34, 0xAB, 0x7C};
    CHECK(bthome::addr_str(le) == "7C:AB:34:12:00:3C");
    CHECK(bthome::normalize_addr("7c-ab-34-12-00-3c") == "7C:AB:34:12:00:3C");
    CHECK(bthome::normalize_addr("7C:AB:34:12:00") == "");
    CHECK(bthome::normalize_addr("7C:AB:34:12:00:3G") == "");
    CHECK(bthome::normalize_addr("7C:AB:34:12:00;3C") == "");
}

static void test_pair_by_button() {
    Tracker t;
    CHECK(!t.wants_scan(0));
    t.start_pairing(100);
    CHECK(t.wants_scan(100));
    CHECK(t.pairing_remaining_s(110) == 50);
    // Other sensors' periodic adverts don't pair, but are listed.
    CHECK(t.on_advert("AA:AA:AA:AA:AA:01", -80, parse(blu_ht(19.0f)), 101) ==
          Tracker::Result::Ignored);
    CHECK(!t.paired());
    // Encrypted press can't pair.
    Reading enc = parse(adv({0x00}, 0x45));
    enc.has_button = true;  // can't actually be decoded; belt and braces
    enc.button = 1;
    CHECK(t.on_advert("AA:AA:AA:AA:AA:02", -40, enc, 102) == Tracker::Result::Ignored);
    CHECK(!t.paired());
    auto nb = t.nearby(103);
    CHECK(nb.size() == 2);
    CHECK(nb[0].addr == "AA:AA:AA:AA:AA:02" && nb[0].encrypted);
    // Button press pairs, keeping the reading in the same advert.
    CHECK(t.on_advert("AA:AA:AA:AA:AA:03", -55, parse(blu_ht(21.4f, 50, 90, 1)), 104) ==
          Tracker::Result::Paired);
    CHECK(t.paired() && t.addr() == "AA:AA:AA:AA:AA:03");
    CHECK(!t.pairing(105));
    CHECK(t.fresh(105));
    CHECK(near(t.sample().temp_c, 21.4f));
    CHECK(t.sample().has_battery && t.sample().battery == 90);
    CHECK(t.sample().rssi == -55);
    // Once paired, another sensor's press is ignored.
    CHECK(t.on_advert("AA:AA:AA:AA:AA:01", -30, parse(blu_ht(18.0f, 50, 90, 1)), 106) ==
          Tracker::Result::Ignored);
    CHECK(t.addr() == "AA:AA:AA:AA:AA:03");
}

static void test_pair_window_expires() {
    Tracker t;
    t.start_pairing(0);
    CHECK(t.on_advert("AA:AA:AA:AA:AA:03", -50, parse(blu_ht(21.0f, 50, 90, 1)),
                      room_sensor::kPairWindowS) == Tracker::Result::Ignored);
    CHECK(!t.paired());
    CHECK(!t.wants_scan(room_sensor::kPairWindowS));
}

static void test_select_from_nearby() {
    Tracker t;
    t.start_pairing(0);
    t.on_advert("AA:AA:AA:AA:AA:05", -60, parse(blu_ht(20.5f)), 5);
    Reading enc = parse(adv({0x00}, 0x41));
    t.on_advert("AA:AA:AA:AA:AA:06", -60, enc, 6);
    CHECK(!t.select("AA:AA:AA:AA:AA:06", 7));  // encrypted
    CHECK(!t.select("AA:AA:AA:AA:AA:07", 7));  // never heard
    CHECK(t.select("AA:AA:AA:AA:AA:05", 7));
    CHECK(t.paired() && !t.pairing(7));
    CHECK(t.fresh(7) && near(t.sample().temp_c, 20.5f));
}

static void test_freshness_and_updates() {
    Tracker t;
    t.set_paired("AA:AA:AA:AA:AA:03", "Bedroom");
    CHECK(t.wants_scan(0));
    CHECK(!t.fresh(0));  // no reading yet
    t.on_advert("AA:AA:AA:AA:AA:03", -60, parse(blu_ht(20.0f)), 10);
    CHECK(t.fresh(10));
    CHECK(t.fresh(10 + room_sensor::kStaleS - 1));
    CHECK(!t.fresh(10 + room_sensor::kStaleS));
    // An advert without temperature keeps the device "seen" but not fresh.
    t.on_advert("AA:AA:AA:AA:AA:03", -62, parse(adv({0x01, 0x40})), 700);
    CHECK(!t.fresh(700));
    CHECK(t.sample().seen_at_s == 700 && t.sample().battery == 0x40);
    t.on_advert("AA:AA:AA:AA:AA:03", -62, parse(blu_ht(20.5f)), 701);
    CHECK(t.fresh(701));
    // Re-setting the same address keeps the sample; a new one clears it.
    t.set_paired("AA:AA:AA:AA:AA:03", "Bedroom");
    CHECK(t.fresh(702));
    t.set_paired("AA:AA:AA:AA:AA:04", "");
    CHECK(!t.fresh(702) && !t.sample().seen);
    t.set_paired("", "x");
    CHECK(!t.paired() && t.name().empty() && !t.wants_scan(703));
}

static void test_nearby_cap_and_ttl() {
    Tracker t;
    t.start_pairing(0);
    for (int i = 0; i < 20; ++i) {
        char a[18];
        std::snprintf(a, sizeof(a), "AA:AA:AA:AA:AA:%02X", i);
        t.on_advert(a, -50 - i, parse(blu_ht(20.0f)), static_cast<uint32_t>(i));
    }
    auto nb = t.nearby(20);
    CHECK(nb.size() == room_sensor::kMaxNearby);
    CHECK(nb.front().rssi >= nb.back().rssi);
    // The oldest were dropped.
    bool has0 = false;
    for (auto& n : nb) has0 |= n.addr == "AA:AA:AA:AA:AA:00";
    CHECK(!has0);
    CHECK(t.nearby(19 + room_sensor::kNearbyTtlS + 1).empty());
}

int main() {
    test_parse_blu_ht();
    test_parse_variants();
    test_addr();
    test_pair_by_button();
    test_pair_window_expires();
    test_select_from_nearby();
    test_freshness_and_updates();
    test_nearby_cap_and_ttl();
    if (g_failures) {
        std::printf("%d room_sensor test(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("All room_sensor tests passed\n");
    return 0;
}
