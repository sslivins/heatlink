// Host-side unit tests for room sensors (#142): BTHome parser, Shelly
// webhook helpers and the per-head sensor set.
//
// Build & run (see .github/workflows/host-tests.yml):
//   g++ -std=c++17 -Wall -Wextra -I components/room_sensor/include
//       components/room_sensor/*.cpp test/host/test_room_sensor.cpp

#include "bthome.h"
#include "sensor_set.h"
#include "shelly_ht.h"

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
using room_sensor::Kind;
using room_sensor::SensorSet;
using R = room_sensor::SensorSet::Result;

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


static std::string addr_n(int i) {
    char b[18];
    std::snprintf(b, sizeof(b), "AA:BB:CC:DD:EE:%02X", i);
    return b;
}

static void test_pair_by_button() {
    SensorSet s;
    const std::string a = "7C:C6:B6:12:34:56";
    // Outside the window a press is ignored and nothing scans.
    CHECK(!s.wants_scan(0));
    CHECK(s.on_advert(a, -60, parse(blu_ht(21.0f, 40, 90, 1)), 5) == R::Ignored);
    CHECK(s.sensors().empty());
    s.start_pairing(10);
    CHECK(s.wants_scan(10) && s.pairing(10) && s.pairing_remaining_s(10) == 60);
    // A reading without a press only lists it nearby.
    CHECK(s.on_advert(a, -60, parse(blu_ht(21.0f)), 11) == R::Ignored);
    CHECK(s.nearby(11).size() == 1);
    // Encrypted presses never pair.
    CHECK(s.on_advert("11:22:33:44:55:66", -50, parse(adv({0x3A, 0x01}, 0x45)), 12) == R::Ignored);
    CHECK(s.on_advert(a, -58, parse(blu_ht(21.5f, 41, 89, 1)), 13) == R::Paired);
    CHECK(s.sensors().size() == 1 && !s.pairing(13));
    const auto& p = s.sensors()[0];
    CHECK(p.id == a && p.kind == Kind::BTHome);
    CHECK(p.sample.valid && near(p.sample.temp_c, 21.5f) && p.sample.battery == 89);
    CHECK(s.wants_scan(100));  // a BTHome sensor keeps the scan running
    // A paired sensor isn't offered again, and its press while pairing just updates it.
    s.start_pairing(20);
    CHECK(s.on_advert(a, -58, parse(blu_ht(22.0f, 41, 89, 1)), 21) == R::Updated);
    CHECK(s.sensors().size() == 1 && s.nearby(21).empty() && s.pairing(21));
}

static void test_pair_window_expires() {
    SensorSet s;
    s.start_pairing(0);
    CHECK(!s.pairing(60) && s.pairing_remaining_s(60) == 0);
    CHECK(s.on_advert(addr_n(1), -60, parse(blu_ht(20.0f, 40, 90, 1)), 61) == R::Ignored);
    CHECK(s.sensors().empty() && !s.wants_scan(61));
}

static void test_select_from_nearby() {
    SensorSet s;
    s.start_pairing(0);
    s.on_advert(addr_n(1), -70, parse(blu_ht(19.5f)), 1);
    s.on_advert(addr_n(2), -50, parse(adv({0x45, 0x00, 0x00}, 0x45)), 2);  // encrypted
    CHECK(s.select(addr_n(2), 3) == R::Ignored);
    CHECK(s.select("00:00:00:00:00:00", 3) == R::Ignored);
    CHECK(s.select(addr_n(1), 3) == R::Paired);
    CHECK(s.sensors().size() == 1 && near(s.sensors()[0].sample.temp_c, 19.5f));
    s.start_pairing(10);
    s.on_advert(addr_n(3), -70, parse(blu_ht(19.5f)), 11);
    CHECK(s.select(addr_n(3), 11 + room_sensor::kNearbyTtlS) == R::Ignored);  // stale
}

static void test_capacity() {
    SensorSet s;
    for (int i = 0; i < 4; ++i) {
        s.start_pairing(i * 100);
        CHECK(s.on_advert(addr_n(i), -60, parse(blu_ht(20.0f, 40, 90, 1)), i * 100 + 1) == R::Paired);
    }
    CHECK(s.full() && s.sensors().size() == room_sensor::kMaxSensors);
    s.start_pairing(1000);
    CHECK(s.on_advert(addr_n(9), -60, parse(blu_ht(20.0f, 40, 90, 1)), 1001) == R::Full);
    room_sensor::Sensor w;
    w.id = "shellyhtg3-x";
    w.kind = Kind::ShellyRpc;
    CHECK(!s.add(w));
    // Replacing an existing id is allowed even when full.
    room_sensor::Sensor same = s.sensors()[0];
    same.name = "Desk";
    CHECK(s.add(same) && s.sensors()[0].name == "Desk" && s.sensors().size() == 4);
    CHECK(s.remove(addr_n(0)) && !s.remove(addr_n(0)) && !s.full());
    CHECK(s.add(w) && s.sensors().size() == 4);
}

static void test_room_temp_average_and_staleness() {
    SensorSet s;
    float t = 0;
    size_t n = 9;
    CHECK(!s.room_temp(0, &t, &n) && n == 0);
    s.start_pairing(0);
    s.on_advert(addr_n(1), -60, parse(blu_ht(20.0f, 40, 90, 1)), 1);
    s.start_pairing(2);
    s.on_advert(addr_n(2), -60, parse(blu_ht(22.0f, 40, 90, 1)), 3);
    room_sensor::Sensor w;
    w.id = "shellyhtg3-aabb";
    w.kind = Kind::ShellyRpc;
    w.token = "tok";
    CHECK(s.add(w));
    CHECK(s.room_temp(10, &t, &n) && n == 2 && near(t, 21.0f));
    shelly::Report r;
    CHECK(shelly::parse_report("k=tok&t=24.5&h=40&b=90&r=-55", r));
    std::string id;
    CHECK(s.on_report(r, 20, &id) == R::Updated && id == "shellyhtg3-aabb");
    CHECK(s.room_temp(20, &t, &n) && n == 3 && near(t, (20.0f + 22.0f + 24.5f) / 3));
    // Wrong token: ignored.
    shelly::Report bad = r;
    bad.token = "nope";
    CHECK(s.on_report(bad, 21) == R::Ignored);
    // Sensor 1 keeps advertising; sensor 2 goes quiet.
    s.on_advert(addr_n(1), -60, parse(blu_ht(20.0f)), 500);
    CHECK(s.room_temp(3 + room_sensor::kBleStaleS, &t, &n) && n == 2 &&
          near(t, (20.0f + 24.5f) / 2));
    // The Wi-Fi sensor survives a 2 h silence; the BLE ones are stale by then.
    CHECK(s.room_temp(20 + 2 * 3600, &t, &n) && n == 1 && near(t, 24.5f));
    CHECK(!s.room_temp(20 + room_sensor::kWifiStaleS, &t, &n) && n == 0);
    // BTHome adverts never update a Shelly sensor.
    bthome::Reading br;
    br.has_temp = true;
    br.temp_c = 30.0f;
    CHECK(s.on_advert("shellyhtg3-aabb", -40, br, 30) == R::Ignored);
    // Only Shelly sensors left: no Bluetooth scan.
    CHECK(s.remove(addr_n(1)) && s.remove(addr_n(2)) && !s.wants_scan(40));
}

static void test_nearby_cap_and_ttl() {
    SensorSet s;
    s.start_pairing(0);
    for (int i = 0; i < 20; ++i) s.on_advert(addr_n(i), -40 - i, parse(blu_ht(20.0f)), i);
    auto nb = s.nearby(19);
    CHECK(nb.size() == room_sensor::kMaxNearby);
    CHECK(nb.front().rssi >= nb.back().rssi);
    bool has0 = false;
    for (auto& e : nb) has0 |= e.addr == addr_n(0);
    CHECK(!has0);
    CHECK(s.nearby(19 + room_sensor::kNearbyTtlS + 1).empty());
}

static void test_kind_strings() {
    Kind k;
    CHECK(std::string(room_sensor::kind_str(Kind::BTHome)) == "bthome");
    CHECK(std::string(room_sensor::kind_str(Kind::ShellyRpc)) == "shelly");
    CHECK(room_sensor::kind_from_str("shelly", k) && k == Kind::ShellyRpc);
    CHECK(room_sensor::kind_from_str("bthome", k) && k == Kind::BTHome);
    CHECK(!room_sensor::kind_from_str("zigbee", k));
}

static void test_shelly_url_and_report() {
    std::string u = shelly::report_url("192.168.1.108", "abc123");
    CHECK(u.rfind("http://192.168.1.108/api/sensors/report?k=abc123&t=${status[", 0) == 0);
    CHECK(u.find("${status[\"humidity:0\"].rh}") != std::string::npos);
    CHECK(u.find("${status[\"devicepower:0\"].battery.percent}") != std::string::npos);

    shelly::Report r;
    CHECK(shelly::parse_report("k=abc123&t=21.53&h=45.2&b=87&r=-61", r));
    CHECK(r.token == "abc123");
    CHECK(r.has_temp && near(r.temp_c, 21.53f));
    CHECK(r.has_humidity && near(r.humidity, 45.2f));
    CHECK(r.has_battery && r.battery == 87);
    CHECK(r.has_rssi && r.rssi == -61);
    // Values Shelly couldn't evaluate are dropped, not misread.
    CHECK(shelly::parse_report("k=x&t=null&h=%24%7Bstatus%7D&b=&r=5", r));
    CHECK(!r.has_temp && !r.has_humidity && !r.has_battery && !r.has_rssi);
    CHECK(shelly::parse_report("t=-3.5&k=tok", r) && r.has_temp && near(r.temp_c, -3.5f));
    CHECK(!shelly::parse_report("t=20", r));
    CHECK(shelly::parse_report("k=a%2Bb&t=20", r) && r.token == "a+b");
    CHECK(shelly::parse_report("k=a&t=200", r) && !r.has_temp);

    CHECK(near(shelly::clamp_step(0.34f), 0.3f));
    CHECK(near(shelly::clamp_step(0.0f), 0.1f));
    CHECK(near(shelly::clamp_step(9.0f), 5.0f));
    CHECK(shelly::is_ht_name("ShellyHTG3-AABBCCDDEEFF"));
    CHECK(shelly::is_ht_name("shellyplusht-441793cc9d30"));
    CHECK(!shelly::is_ht_name("shellyplus1pm-abc"));
    CHECK(shelly::token_equal("abc", "abc"));
    CHECK(!shelly::token_equal("abc", "abd"));
    CHECK(!shelly::token_equal("abc", "abcd"));
    CHECK(!shelly::token_equal("", ""));
}

int main() {
    test_parse_blu_ht();
    test_parse_variants();
    test_addr();
    test_pair_by_button();
    test_pair_window_expires();
    test_select_from_nearby();
    test_capacity();
    test_room_temp_average_and_staleness();
    test_nearby_cap_and_ttl();
    test_kind_strings();
    test_shelly_url_and_report();
    if (g_failures) {
        std::printf("%d room_sensor test(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("All room_sensor tests passed\n");
    return 0;
}
