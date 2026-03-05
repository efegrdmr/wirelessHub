#include "wifi_manager.h"
#include "hub_client.h"

#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "Main";

void app_main(void)
{
    // NVS must be initialised before anything reads or writes it.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition was truncated or upgraded — erase and retry.
        ESP_LOGW(TAG, "NVS partition issue (%s), erasing...", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    ESP_LOGI(TAG, "Starting WiFi manager");
    wifi_manager_start();

    // Discover the daemon on the local network as soon as WiFi is ready.
    // hub_client_start() spawns a FreeRTOS task that waits for WIFI_READY_BIT
    // internally, so it is safe to call here unconditionally.
    hub_client_start();
}
