/// @file factory_reset.cpp
/// @brief Full-wipe factory reset (software-triggered).
#include "factory_reset.h"

#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace factory_reset {
namespace {
const char* TAG = "factory_reset";
}  // namespace

void perform() {
    ESP_LOGW(TAG, "FACTORY RESET: erasing all NVS and rebooting into setup mode");
    // Release the initialized default partition before erasing it. Any handle
    // is opened/closed per-op elsewhere, so nothing holds it open across this.
    nvs_flash_deinit();
    esp_err_t err = nvs_flash_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_erase failed: %s", esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    for (;;) {}  // unreachable; satisfies [[noreturn]]
}

}  // namespace factory_reset
