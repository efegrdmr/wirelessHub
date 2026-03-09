#include "eth_passthrough.h"
#include "eth_driver.h"
#include "device_session.h"
#include "Protocol.h"

#include "esp_log.h"

static const char *TAG = "EthPass";

void eth_passthrough_start(void)
{
    ESP_LOGI(TAG, "Starting Ethernet passthrough (device_id=0x%02X)", DEVICE_ID_ETHERNET);

    static const device_session_cfg_t eth_session_cfg = {
        .device_id = DEVICE_ID_ETHERNET,
        .speed     = 0x00,   /* not applicable for Ethernet */
        .usb_class = 0x00,   /* not a USB device */
        .subclass  = 0x00,
        .protocol  = 0x00,
        .ops       = &ETH_DRIVER_OPS,
    };

    device_session_start(&eth_session_cfg);
}
