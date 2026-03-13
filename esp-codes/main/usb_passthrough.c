#include "usb_passthrough.h"
#include "usb_driver.h"
#include "device_session.h"
#include "Protocol.h"

#include "esp_log.h"

static const char *TAG = "UsbPass";

void usb_passthrough_start(void)
{
    ESP_LOGI(TAG, "Starting USB passthrough (device_id=0x%02X)", DEVICE_ID_USB0);

    static const device_session_cfg_t usb_session_cfg = {
        .device_id  = DEVICE_ID_USB0,
        .speed      = USB_SPEED_FULL,   /* updated to actual speed after enum */
        .usb_class  = 0x00,             /* filled in after enumeration        */
        .subclass   = 0x00,
        .protocol   = 0x00,
        .usbip_mode = true,             /* daemon speaks USB/IP; ESP proxies  */
        .ops        = &USB_DRIVER_OPS,
    };

    device_session_start(&usb_session_cfg);
}
