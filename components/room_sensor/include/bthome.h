/// @file bthome.h
/// @brief BTHome v2 advertisement parser (https://bthome.io/format/) —
/// pure, host-testable. Used for Shelly BLU H&T room sensors (#142).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace bthome {

/// BTHome's 16-bit service UUID (service data AD type 0x16).
constexpr uint16_t kServiceUuid = 0xFCD2;

/// Button event values (object 0x3A).
enum Button : uint8_t {
    kButtonNone = 0x00,
    kButtonPress = 0x01,
    kButtonDouble = 0x02,
    kButtonTriple = 0x03,
    kButtonLong = 0x04,
};

struct Reading {
    bool    encrypted = false;   ///< payload is AES-CCM encrypted (not decoded)
    bool    trigger = false;     ///< sent irregularly (button / on change)
    bool    has_packet_id = false;
    uint8_t packet_id = 0;
    bool    has_temp = false;
    float   temp_c = 0.0f;
    bool    has_humidity = false;
    float   humidity = 0.0f;     ///< %RH
    bool    has_battery = false;
    uint8_t battery = 0;         ///< %
    bool    has_button = false;
    uint8_t button = kButtonNone;
    std::string name;            ///< local name AD (0x08/0x09), if present
};

/// Parse a raw advertisement (a sequence of length/type/data AD structures).
/// Returns true if it carries a BTHome v2 service-data block; `out` then holds
/// whatever objects were decoded (an unknown object id ends decoding, keeping
/// the objects before it). Encrypted payloads set `encrypted` only.
bool parse(const uint8_t* adv, size_t len, Reading& out);

/// "AA:BB:CC:DD:EE:FF" from a NimBLE address (val[] is little-endian:
/// val[5] is the first, most-significant byte).
std::string addr_str(const uint8_t val_le[6]);

/// Canonical upper-case "AA:BB:CC:DD:EE:FF" (accepts lower case and '-'),
/// or "" if malformed.
std::string normalize_addr(const std::string& s);

}  // namespace bthome
