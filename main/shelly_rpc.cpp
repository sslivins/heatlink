/// @file shelly_rpc.cpp
/// @brief See shelly_rpc.h.

#include "shelly_rpc.h"
#include "shelly_ht.h"

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "mdns.h"

namespace shelly_rpc {
namespace {
const char* TAG = "shelly";
constexpr int kMaxBody = 16 * 1024;
}  // namespace

bool call(const std::string& host, const char* method, cJSON* params,
          cJSON** result, std::string* err, int timeout_ms) {
    if (result) *result = nullptr;
    cJSON* req = cJSON_CreateObject();
    cJSON_AddNumberToObject(req, "id", 1);
    cJSON_AddStringToObject(req, "method", method);
    if (params) cJSON_AddItemToObject(req, "params", params);
    char* body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) {
        if (err) *err = "out of memory";
        return false;
    }

    const std::string url = "http://" + host + "/rpc";
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = timeout_ms;
    cfg.disable_auto_redirect = true;
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        cJSON_free(body);
        if (err) *err = "http client init failed";
        return false;
    }
    esp_http_client_set_header(cli, "Content-Type", "application/json");
    const int len = static_cast<int>(strlen(body));
    std::string resp;
    int status = 0;
    bool io_ok = esp_http_client_open(cli, len) == ESP_OK &&
                 esp_http_client_write(cli, body, len) == len &&
                 esp_http_client_fetch_headers(cli) >= 0;
    cJSON_free(body);
    if (io_ok) {
        status = esp_http_client_get_status_code(cli);
        char buf[512];
        int n;
        while ((n = esp_http_client_read(cli, buf, sizeof(buf))) > 0 &&
               static_cast<int>(resp.size()) < kMaxBody)
            resp.append(buf, n);
    }
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    if (!io_ok) {
        if (err) *err = "sensor not reachable (is it awake?)";
        return false;
    }
    if (status == 401) {
        if (err) *err = "the sensor has a password; turn off authentication in the Shelly app";
        return false;
    }

    cJSON* j = cJSON_Parse(resp.c_str());
    if (!j) {
        if (err) *err = "unexpected reply (HTTP " + std::to_string(status) + ")";
        return false;
    }
    bool ok = false;
    cJSON* r = cJSON_DetachItemFromObject(j, "result");
    if (r) {
        ok = true;
        if (result) *result = r;
        else cJSON_Delete(r);
    } else if (err) {
        const cJSON* e = cJSON_GetObjectItem(j, "error");
        const cJSON* m = e ? cJSON_GetObjectItem(e, "message") : nullptr;
        *err = (m && cJSON_IsString(m)) ? m->valuestring
                                        : "RPC failed (HTTP " + std::to_string(status) + ")";
    }
    cJSON_Delete(j);
    if (!ok) ESP_LOGW(TAG, "%s %s failed", host.c_str(), method);
    return ok;
}

std::vector<Found> discover(uint32_t timeout_ms) {
    std::vector<Found> out;
    mdns_result_t* res = nullptr;
    if (mdns_query_ptr("_shelly", "_tcp", timeout_ms, 20, &res) != ESP_OK) return out;
    for (mdns_result_t* r = res; r; r = r->next) {
        std::string name = r->instance_name ? r->instance_name
                                            : (r->hostname ? r->hostname : "");
        if (!shelly::is_ht_name(name) &&
            !(r->hostname && shelly::is_ht_name(r->hostname)))
            continue;
        std::string ip;
        for (mdns_ip_addr_t* a = r->addr; a; a = a->next) {
            if (a->addr.type != ESP_IPADDR_TYPE_V4) continue;
            char b[16];
            esp_ip4addr_ntoa(&a->addr.u_addr.ip4, b, sizeof(b));
            ip = b;
            break;
        }
        if (ip.empty()) continue;
        bool dup = false;
        for (const Found& f : out) dup |= f.ip == ip;
        if (!dup) out.push_back(Found{name, ip});
    }
    mdns_query_results_free(res);
    return out;
}

std::string own_ip() {
    esp_netif_t* n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {};
    if (!n || esp_netif_get_ip_info(n, &ip) != ESP_OK || ip.ip.addr == 0) return "";
    char b[16];
    esp_ip4addr_ntoa(&ip.ip, b, sizeof(b));
    return b;
}

}  // namespace shelly_rpc
