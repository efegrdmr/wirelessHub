#include "wifi_manager.h"
#include "hub_client.h"
#include "log_forwarder.h"
#include "crash_reporter.h"
#include "eth_passthrough.h"
#include "usb_passthrough.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_system.h"

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

    /* Small delay so the serial monitor has time to connect before
       early init logs scroll past. Remove after debugging. */
    vTaskDelay(pdMS_TO_TICKS(2000));

    ESP_LOGI(TAG, "Starting WiFi manager");
    wifi_manager_start();

    hub_client_start();

    // log_forwarder must come before crash_reporter so crash reports go to daemon
    log_forwarder_init();
    ESP_LOGI(TAG, "Reboot detected. Reset reason: %d", esp_reset_reason());
    crash_reporter_init();

    eth_passthrough_start();
    usb_passthrough_start();
}
