/// @file shelly_ht.cpp
/// @brief Shelly Wi-Fi H&T helpers (see shelly_ht.h). No ESP-IDF deps.

#include "shelly_ht.h"

#include <cctype>
#include <cmath>
#include <cstdlib>

namespace shelly {

std::string report_url(const std::string& head_ip, const std::string& token) {
    return "http://" + head_ip + kReportPath + "?k=" + token +
           "&t=${status[\"temperature:0\"].tC}"
           "&h=${status[\"humidity:0\"].rh}"
           "&b=${status[\"devicepower:0\"].battery.percent}"
           "&r=${status.wifi.rssi}";
}

namespace {

int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string url_decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out += ' ';
        } else if (s[i] == '%' && i + 2 < s.size() && hexval(s[i + 1]) >= 0 &&
                   hexval(s[i + 2]) >= 0) {
            out += static_cast<char>(hexval(s[i + 1]) * 16 + hexval(s[i + 2]));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

bool to_num(const std::string& s, float lo, float hi, float& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    float v = std::strtof(s.c_str(), &end);
    if (end != s.c_str() + s.size() || !std::isfinite(v) || v < lo || v > hi) return false;
    out = v;
    return true;
}

}  // namespace

bool parse_report(const std::string& query, Report& out) {
    out = Report{};
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        const std::string kv = query.substr(pos, amp - pos);
        pos = amp + 1;
        const size_t eq = kv.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = kv.substr(0, eq);
        const std::string v = url_decode(kv.substr(eq + 1));
        float f = 0.0f;
        if (k == "k") {
            out.token = v;
        } else if (k == "t" && to_num(v, -40.0f, 80.0f, f)) {
            out.has_temp = true;
            out.temp_c = f;
        } else if (k == "h" && to_num(v, 0.0f, 100.0f, f)) {
            out.has_humidity = true;
            out.humidity = f;
        } else if (k == "b" && to_num(v, 0.0f, 100.0f, f)) {
            out.has_battery = true;
            out.battery = static_cast<int>(std::lround(f));
        } else if (k == "r" && to_num(v, -127.0f, 0.0f, f)) {
            out.has_rssi = true;
            out.rssi = static_cast<int>(std::lround(f));
        }
    }
    return !out.token.empty();
}

float clamp_step(float c) {
    if (!std::isfinite(c)) return kDefaultStepC;
    if (c < kMinStepC) c = kMinStepC;
    if (c > kMaxStepC) c = kMaxStepC;
    return std::round(c * 10.0f) / 10.0f;
}

bool is_ht_name(const std::string& name) {
    std::string l;
    for (char c : name) l += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return l.rfind("shellyht", 0) == 0 || l.rfind("shellyplusht", 0) == 0;
}

bool token_equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size() || a.empty()) return false;
    unsigned char d = 0;
    for (size_t i = 0; i < a.size(); ++i)
        d |= static_cast<unsigned char>(a[i] ^ b[i]);
    return d == 0;
}

}  // namespace shelly
