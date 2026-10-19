/// @file shelly_rpc.h
/// @brief Minimal Shelly Gen2+ RPC client and mDNS discovery (#142).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct cJSON;

namespace shelly_rpc {

/// POST http://<host>/rpc {"id":1,"method":..,"params":..}. Takes ownership
/// of params (may be null). On success *result holds the "result" object
/// (caller cJSON_Delete()s it); otherwise err says why.
bool call(const std::string& host, const char* method, cJSON* params,
          cJSON** result, std::string* err, int timeout_ms = 4000);

struct Found {
    std::string name;  ///< mDNS instance/host name, e.g. "ShellyHTG3-AABBCCDDEEFF"
    std::string ip;
};
/// Shelly H&Ts answering `_shelly._tcp` (only while awake).
std::vector<Found> discover(uint32_t timeout_ms);

/// This head's station IPv4 ("" if not connected).
std::string own_ip();

}  // namespace shelly_rpc
