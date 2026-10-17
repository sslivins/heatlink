/// @file bthome.cpp
/// @brief BTHome v2 parser (see bthome.h). No ESP-IDF deps.

#include "bthome.h"

#include <cctype>
#include <cstdio>

namespace bthome {

namespace {

constexpr uint8_t kAdServiceData16 = 0x16;
constexpr uint8_t kAdShortName = 0x08;
constexpr uint8_t kAdCompleteName = 0x09;

// Payload size of a BTHome v2 object id: >0 fixed, 0 variable-length (first
// byte is the length), -1 unknown (decoding must stop: we can't skip it).
int object_size(uint8_t id) {
    if (id >= 0x15 && id <= 0x2D) return 1;  // binary sensors
    switch (id) {
        case 0x00: case 0x01: case 0x09: case 0x0F: case 0x10: case 0x11:
        case 0x2E: case 0x2F: case 0x3A: case 0x46: case 0x57: case 0x58:
        case 0x59: case 0x60:
            return 1;
        case 0x02: case 0x03: case 0x06: case 0x07: case 0x08: case 0x0C:
        case 0x0D: case 0x0E: case 0x12: case 0x13: case 0x14: case 0x3C:
        case 0x3D: case 0x3F: case 0x40: case 0x41: case 0x43: case 0x44:
        case 0x45: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x51:
        case 0x52: case 0x56: case 0x5A: case 0x5D: case 0x5E: case 0x5F:
        case 0x61: case 0xF0:
            return 2;
        case 0x04: case 0x05: case 0x0A: case 0x0B: case 0x42: case 0x4B:
        case 0xF2:
            return 3;
        case 0x3E: case 0x4C: case 0x4D: case 0x4E: case 0x4F: case 0x50:
        case 0x55: case 0x5B: case 0x5C: case 0xF1:
            return 4;
        case 0x53: case 0x54:
            return 0;
        default:
            return -1;
    }
}

int16_t s16(const uint8_t* p) { return static_cast<int16_t>(p[0] | (p[1] << 8)); }
uint16_t u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

void decode_objects(const uint8_t* p, size_t n, Reading& out) {
    size_t i = 0;
    while (i < n) {
        const uint8_t id = p[i++];
        int sz = object_size(id);
        if (sz < 0) return;
        if (sz == 0) {
            if (i >= n) return;
            sz = p[i++];
        }
        if (i + static_cast<size_t>(sz) > n) return;
        const uint8_t* v = p + i;
        switch (id) {
            case 0x00:
                out.has_packet_id = true;
                out.packet_id = v[0];
                break;
            case 0x01:
                out.has_battery = true;
                out.battery = v[0] > 100 ? 100 : v[0];
                break;
            case 0x02:
                out.has_temp = true;
                out.temp_c = s16(v) * 0.01f;
                break;
            case 0x45:
                out.has_temp = true;
                out.temp_c = s16(v) * 0.1f;
                break;
            case 0x57:
                out.has_temp = true;
                out.temp_c = static_cast<int8_t>(v[0]);
                break;
            case 0x58:
                out.has_temp = true;
                out.temp_c = static_cast<int8_t>(v[0]) * 0.35f;
                break;
            case 0x03:
                out.has_humidity = true;
                out.humidity = u16(v) * 0.01f;
                break;
            case 0x2E:
                out.has_humidity = true;
                out.humidity = v[0];
                break;
            case 0x3A:
                if (!out.has_button) {  // multi-button devices: first one
                    out.has_button = true;
                    out.button = v[0];
                }
                break;
            default:
                break;
        }
        i += static_cast<size_t>(sz);
    }
}

}  // namespace

bool parse(const uint8_t* adv, size_t len, Reading& out) {
    out = Reading{};
    bool found = false;
    size_t i = 0;
    while (i < len) {
        const uint8_t ad_len = adv[i];
        if (ad_len == 0) break;
        if (i + 1 + ad_len > len) break;
        const uint8_t type = adv[i + 1];
        const uint8_t* d = adv + i + 2;
        const size_t dn = ad_len - 1;
        if ((type == kAdCompleteName || (type == kAdShortName && out.name.empty())) && dn > 0) {
            out.name.assign(reinterpret_cast<const char*>(d), dn);
        } else if (type == kAdServiceData16 && dn >= 3 && !found &&
                   u16(d) == kServiceUuid) {
            const uint8_t info = d[2];
            if ((info >> 5) == 2) {  // BTHome v2 only
                found = true;
                out.encrypted = (info & 0x01) != 0;
                out.trigger = (info & 0x04) != 0;
                if (!out.encrypted) decode_objects(d + 3, dn - 3, out);
            }
        }
        i += 1 + ad_len;
    }
    return found;
}

std::string addr_str(const uint8_t val_le[6]) {
    char buf[18];
    std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", val_le[5], val_le[4],
                  val_le[3], val_le[2], val_le[1], val_le[0]);
    return buf;
}

std::string normalize_addr(const std::string& s) {
    if (s.size() != 17) return "";
    std::string o(17, ':');
    for (size_t i = 0; i < 17; ++i) {
        const char c = s[i];
        if (i % 3 == 2) {
            if (c != ':' && c != '-') return "";
            continue;
        }
        if (!std::isxdigit(static_cast<unsigned char>(c))) return "";
        o[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return o;
}

}  // namespace bthome
