/// @file token_store.cpp
#include "token_store.h"

#include <mutex>
#include <time.h>

#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "psa/crypto.h"

static const char* TAG = "tokens";
static const char* kNs = "auth";
static const char* kKey = "tokens";

static std::mutex s_mtx;
static tokens::Table s_table;
static bool s_loaded = false;

// Wall-clock seconds, or 0 before SNTP has set the clock.
static uint32_t now_unix() {
    time_t t = time(nullptr);
    return t > 1600000000 ? (uint32_t)t : 0;
}

static bool hash_secret(const std::string& secret, tokens::Hash* out) {
    size_t len = 0;
    return psa_crypto_init() == PSA_SUCCESS &&
           psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t*)secret.data(), secret.size(),
                            out->data(), out->size(), &len) == PSA_SUCCESS &&
           len == out->size();
}

static std::string random_hex(size_t digits) {
    std::vector<uint8_t> b(digits / 2);
    esp_fill_random(b.data(), b.size());
    return tokens::to_hex(b.data(), b.size());
}

// Caller holds s_mtx.
static bool save_locked() {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return false;
    std::vector<uint8_t> blob = s_table.serialize();
    esp_err_t err = nvs_set_blob(h, kKey, blob.data(), blob.size());
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

static void load_locked() {
    if (s_loaded) return;
    s_loaded = true;
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = 0;
    if (nvs_get_blob(h, kKey, nullptr, &len) == ESP_OK && len > 0) {
        std::vector<uint8_t> blob(len);
        if (nvs_get_blob(h, kKey, blob.data(), &len) == ESP_OK && !s_table.deserialize(blob))
            ESP_LOGW(TAG, "stored tokens unreadable; ignoring");
    }
    nvs_close(h);
    ESP_LOGI(TAG, "%u integration token(s)", (unsigned)s_table.entries().size());
}

void token_store_init() {
    std::lock_guard<std::mutex> g(s_mtx);
    load_locked();
}

bool token_store_create(const std::string& label, tokens::Scope scope, std::string* token_out,
                        tokens::Entry* entry_out, bool replace) {
    std::lock_guard<std::mutex> g(s_mtx);
    load_locked();
    tokens::Entry e;
    e.label = tokens::sanitize_label(label);
    if (e.label.empty()) return false;
    const tokens::Table backup = s_table;
    if (replace) s_table.remove_label(e.label, scope);
    if (s_table.full()) {
        s_table = backup;
        return false;
    }
    do e.id = random_hex(tokens::kIdLen);
    while (s_table.has_id(e.id));
    std::string secret = random_hex(tokens::kSecretLen);
    e.scope = scope;
    e.created = now_unix();
    if (!hash_secret(secret, &e.hash) || !s_table.add(e) || !save_locked()) {
        s_table = backup;
        return false;
    }
    *token_out = tokens::format_token(e.id, secret);
    *entry_out = e;
    ESP_LOGI(TAG, "created %s (%s) \"%s\"", e.id.c_str(), tokens::scope_str(scope),
             e.label.c_str());
    return true;
}

bool token_store_revoke(const std::string& id) {
    std::lock_guard<std::mutex> g(s_mtx);
    load_locked();
    if (!s_table.remove(id)) return false;
    save_locked();
    ESP_LOGI(TAG, "revoked %s", id.c_str());
    return true;
}

void token_store_revoke_all() {
    std::lock_guard<std::mutex> g(s_mtx);
    load_locked();
    if (s_table.entries().empty()) return;
    s_table.clear();
    save_locked();
    ESP_LOGI(TAG, "revoked all tokens");
}

std::vector<tokens::Entry> token_store_list() {
    std::lock_guard<std::mutex> g(s_mtx);
    load_locked();
    return s_table.entries();
}

bool token_store_validate(const std::string& token, tokens::Entry* out) {
    std::string id, secret;
    if (!tokens::parse_token(token, &id, &secret)) return false;
    tokens::Hash h;
    if (!hash_secret(secret, &h)) return false;
    std::lock_guard<std::mutex> g(s_mtx);
    load_locked();
    const tokens::Entry* e = s_table.match(id, h);
    if (!e) return false;
    *out = *e;
    uint32_t now = now_unix();
    if (now && s_table.touch(id, now)) save_locked();
    return true;
}
