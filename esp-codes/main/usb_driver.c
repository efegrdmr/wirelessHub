#include "usb_driver.h"

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "usb/usb_host.h"
#include "esp_private/usb_phy.h"
#include "soc/usb_wrap_struct.h"
#include "soc/usb_dwc_struct.h"

#include <string.h>

static const char *TAG = "UsbDrv";

/* ── Internal state ──────────────────────────────────────────────────────── */

typedef struct {
    uint8_t  data[USB_FRAME_MAX];
    uint16_t len;
} usb_frame_t;

static QueueHandle_t            s_rx_queue  = NULL;
static usb_host_client_handle_t s_client    = NULL;
static usb_device_handle_t      s_dev       = NULL;
static usb_transfer_t          *s_in_xfer   = NULL;
static usb_transfer_t          *s_out_xfer  = NULL;
static usb_transfer_t          *s_ctrl_xfer = NULL;
static uint8_t  s_in_ep    = 0;
static uint8_t  s_out_ep   = 0;
static uint16_t s_in_mps   = 64;
static uint16_t s_out_mps  = 64;
static uint8_t  s_intf_num = 0;

/* Hub tracking */
static volatile bool       s_hub_connected = false;
static usb_device_handle_t s_hub_dev       = NULL;

/* USB PHY handle — must be kept alive for the lifetime of the USB stack */
static usb_phy_handle_t    s_phy_handle    = NULL;

/* Semaphores + status for blocking OUT and control transfers */
static SemaphoreHandle_t s_out_done   = NULL;
static SemaphoreHandle_t s_ctrl_done  = NULL;
static volatile int      s_out_status  = 0;
static volatile int      s_ctrl_status = 0;
static volatile int      s_ctrl_actual = 0;

/* ── Transfer callbacks ──────────────────────────────────────────────────── */

static void in_transfer_cb(usb_transfer_t *xfer)
{
    if (xfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        usb_frame_t frame;
        size_t copy = ((size_t)xfer->actual_num_bytes < USB_FRAME_MAX)
                      ? (size_t)xfer->actual_num_bytes : USB_FRAME_MAX;
        if (copy > 0) {
            memcpy(frame.data, xfer->data_buffer, copy);
        }
        frame.len = (uint16_t)copy;
        xQueueSend(s_rx_queue, &frame, 0);
    } else if (xfer->status != USB_TRANSFER_STATUS_NO_DEVICE) {
        /* If NAK or STALL, push a special frame to wake up the blocked recv thread */
        /* Since len=0 is a valid ZLP, we should use a special length (e.g., 0xFFFF) to indicate an error, 
           or just return len=0 and let the host retry. We will return 0 and error out in recv if we want. */
        usb_frame_t empty = { .len = 0xFFFF };
        xQueueSend(s_rx_queue, &empty, 0);
    }
}

static void out_transfer_cb(usb_transfer_t *xfer)
{
    s_out_status = (int)xfer->status;
    xSemaphoreGive(s_out_done);
}

static void ctrl_transfer_cb(usb_transfer_t *xfer)
{
    s_ctrl_status = (int)xfer->status;
    /* actual_num_bytes includes the 8-byte setup packet for EP0 */
    s_ctrl_actual = (xfer->actual_num_bytes > 8) ? (int)(xfer->actual_num_bytes - 8) : 0;
    xSemaphoreGive(s_ctrl_done);
}

/* ── Endpoint enumeration ────────────────────────────────────────────────── */

static void open_pipes(void)
{
    const usb_config_desc_t *cfg_desc;
    usb_host_get_active_config_descriptor(s_dev, &cfg_desc);

    /* Find first interface */
    int offset = 0;
    const usb_intf_desc_t *intf = (const usb_intf_desc_t *)
        usb_parse_next_descriptor_of_type(
            (const usb_standard_desc_t *)cfg_desc,
            cfg_desc->wTotalLength,
            USB_B_DESCRIPTOR_TYPE_INTERFACE, &offset);
    if (!intf) {
        ESP_LOGE(TAG, "No interface descriptor found — releasing device");
        usb_host_device_close(s_client, s_dev);
        s_dev = NULL;
        return;
    }
    s_intf_num = intf->bInterfaceNumber;

    /* Walk endpoint descriptors */
    offset = 0;
    const usb_standard_desc_t *desc = (const usb_standard_desc_t *)cfg_desc;
    while ((desc = usb_parse_next_descriptor_of_type(
                       desc, cfg_desc->wTotalLength,
                       USB_B_DESCRIPTOR_TYPE_ENDPOINT, &offset)) != NULL) {
        const usb_ep_desc_t *ep = (const usb_ep_desc_t *)desc;
        bool    is_in = (ep->bEndpointAddress & 0x80u) != 0;
        uint8_t type  = ep->bmAttributes & 0x03u;  /* transfer type bits */

        if (!s_in_ep && is_in &&
            (type == USB_BM_ATTRIBUTES_XFER_INT || type == USB_BM_ATTRIBUTES_XFER_BULK)) {
            s_in_ep  = ep->bEndpointAddress;
            s_in_mps = ep->wMaxPacketSize;
            ESP_LOGI(TAG, "IN  ep=0x%02X mps=%u type=%s",
                     s_in_ep, s_in_mps,
                     type == USB_BM_ATTRIBUTES_XFER_INT ? "INT" : "BULK");
        }
        if (!s_out_ep && !is_in &&
            (type == USB_BM_ATTRIBUTES_XFER_INT || type == USB_BM_ATTRIBUTES_XFER_BULK)) {
            s_out_ep  = ep->bEndpointAddress;
            s_out_mps = ep->wMaxPacketSize;
            ESP_LOGI(TAG, "OUT ep=0x%02X mps=%u", s_out_ep, s_out_mps);
        }
    }

    if (!s_in_ep) {
        ESP_LOGE(TAG, "No suitable IN endpoint found");
        return;
    }

    usb_host_interface_claim(s_client, s_dev, s_intf_num, 0);

    usb_host_transfer_alloc(s_in_mps, 0, &s_in_xfer);
    s_in_xfer->device_handle    = s_dev;
    s_in_xfer->bEndpointAddress = s_in_ep;
    s_in_xfer->num_bytes        = s_in_mps;
    s_in_xfer->callback         = in_transfer_cb;
    s_in_xfer->context          = NULL;
    /* Do NOT auto-submit here. We will submit on-demand in usb_drv_recv()
     * to prevent flooding Bulk IN endpoints and causing immediate disconnects. */

    if (s_out_ep) {
        usb_host_transfer_alloc(s_out_mps, 0, &s_out_xfer);
        s_out_xfer->device_handle    = s_dev;
        s_out_xfer->bEndpointAddress = s_out_ep;
        s_out_xfer->callback         = out_transfer_cb;
        s_out_xfer->context          = NULL;
    }

    /* EP0 control transfer buffer — setup(8) + max data (1024) */
    usb_host_transfer_alloc(8 + 1024, 0, &s_ctrl_xfer);
    s_ctrl_xfer->device_handle    = s_dev;
    s_ctrl_xfer->bEndpointAddress = 0;  /* EP0 */
    s_ctrl_xfer->callback         = ctrl_transfer_cb;
    s_ctrl_xfer->context          = NULL;
}


/* ── Client event callback (called inside usb_host_client_handle_events) ── */

static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    (void)arg;

    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        ESP_LOGI(TAG, "NEW_DEV event — addr=%d", msg->new_dev.address);

        usb_device_handle_t tmp_dev;
        esp_err_t err = usb_host_device_open(s_client, msg->new_dev.address, &tmp_dev);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "usb_host_device_open failed: %s", esp_err_to_name(err));
            return;
        }

        const usb_device_desc_t *dev_desc;
        usb_host_get_device_descriptor(tmp_dev, &dev_desc);
        ESP_LOGI(TAG, "USB device connected (addr=%d) VID=0x%04X PID=0x%04X class=0x%02X",
                 msg->new_dev.address,
                 dev_desc->idVendor, dev_desc->idProduct,
                 dev_desc->bDeviceClass);

        if (dev_desc->bDeviceClass == 0x09) {
            /* USB Hub — keep the handle OPEN so the ESP-IDF internal hub
             * driver can continue to monitor the hub's status-change endpoint
             * and enumerate downstream ports.  Closing it here kills
             * downstream device detection. */
            ESP_LOGI(TAG, "USB Hub detected (addr=%d) — keeping handle open for downstream enum",
                     msg->new_dev.address);
            s_hub_dev       = tmp_dev;
            s_hub_connected = true;
        } else {
            /* Real downstream device */
            ESP_LOGI(TAG, "Downstream device (addr=%d) — opening pipes",
                     msg->new_dev.address);
            s_dev = tmp_dev;
            open_pipes();
        }

    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        ESP_LOGW(TAG, "DEV_GONE event");
        if (s_dev) {
            /* Downstream device unplugged */
            ESP_LOGW(TAG, "Downstream device unplugged — tearing down pipes");
            if (s_in_xfer) {
                usb_host_endpoint_halt(s_dev, s_in_ep);
                usb_host_endpoint_flush(s_dev, s_in_ep);
                usb_host_transfer_free(s_in_xfer);
                s_in_xfer = NULL;
            }
            if (s_out_xfer) {
                usb_host_transfer_free(s_out_xfer);
                s_out_xfer = NULL;
            }
            if (s_ctrl_xfer) {
                usb_host_transfer_free(s_ctrl_xfer);
                s_ctrl_xfer = NULL;
            }
            usb_host_interface_release(s_client, s_dev, s_intf_num);
            usb_host_device_close(s_client, s_dev);
            s_dev    = NULL;
            s_in_ep  = 0;
            s_out_ep = 0;
        } else if (s_hub_dev) {
            /* Hub itself was unplugged */
            ESP_LOGW(TAG, "USB Hub unplugged — closing hub handle");
            usb_host_device_close(s_client, s_hub_dev);
            s_hub_dev       = NULL;
            s_hub_connected = false;
        }
    }
}

/* ── Two required tasks for ESP-IDF USB Host ─────────────────────────────── */

/* Task 1: drives USB hardware interrupt handling */
static void usb_lib_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "usb_lib_task running — processing USB hardware events");
    while (true) {
        uint32_t event_flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags) {
            ESP_LOGI(TAG, "usb_lib event_flags=0x%08lX", (unsigned long)event_flags);

            if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
                ESP_LOGW(TAG, "NO_CLIENTS — clearing unowned devices");
                usb_host_device_free_all();
            }
            if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
                ESP_LOGW(TAG, "ALL_FREE — USB bus will re-detect on next plug-in");
            }
        }
    }
}

/* Task 3: watchdog — diagnoses USB host state with HW register reads */
static void usb_watchdog_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));

        /* Read the DWC OTG Host Port register for physical line status */
        uint32_t hprt = USB_DWC.hprt_reg.val;
        bool port_conn   = (hprt >> 0) & 1;  /* bit 0:  PrtConnSts  */
        bool port_ena    = (hprt >> 2) & 1;  /* bit 2:  PrtEna      */
        bool port_pwr    = (hprt >> 12) & 1; /* bit 12: PrtPwr      */
        uint8_t line_sts = (hprt >> 10) & 3; /* bit 11:10 line state */
        uint8_t speed    = (hprt >> 17) & 3; /* bit 18:17 PrtSpd    */

        /* Read USB_WRAP for PHY selection */
        uint32_t wrap_otg = USB_WRAP.otg_conf.val;
        bool phy_sel_internal = (wrap_otg >> 2) & 1; /* phy_sel bit */

        if (!s_hub_connected && !s_dev) {
            ESP_LOGW(TAG, "No app-level device open | HPRT=0x%08lX conn=%d ena=%d pwr=%d "
                     "lineState=%d speed=%d | PHY_internal=%d | heap=%lu",
                     (unsigned long)hprt, port_conn, port_ena, port_pwr,
                     line_sts, speed, phy_sel_internal,
                     (unsigned long)esp_get_free_heap_size());
        } else if (s_hub_connected && !s_dev) {
            ESP_LOGW(TAG, "Hub open, no app-level downstream device | HPRT=0x%08lX",
                     (unsigned long)hprt);
        }

        /* Let's see what the ESP-IDF backend ACTUALLY sees enumerated! */
        int num_devs = 0;
        uint8_t dev_addr_list[10];
        if (usb_host_device_addr_list_fill(10, dev_addr_list, &num_devs) == ESP_OK) {
            if (num_devs > 0) {
                ESP_LOGW(TAG, "--- BACKEND ENUMERATED DEVICES: %d ---", num_devs);
                for (int i = 0; i < num_devs; i++) {
                    ESP_LOGW(TAG, "  Device Address: %d", dev_addr_list[i]);
                }
            } else {
                ESP_LOGW(TAG, "--- BACKEND ENUMERATED DEVICES: 0 ---");
            }
        }
    }
}

/* Task 2: drives client_event_cb via usb_host_client_handle_events() */
static void usb_client_task(void *arg)
{
    (void)arg;
    while (true) {
        usb_host_client_handle_events(s_client, portMAX_DELAY);
    }
}

/* ── device_ops_t implementation ─────────────────────────────────────────── */

static bool usb_drv_init(void)
{
    s_rx_queue = xQueueCreate(USB_RX_QUEUE_DEPTH, sizeof(usb_frame_t));
    if (!s_rx_queue) {
        ESP_LOGE(TAG, "queue create failed");
        return false;
    }

    s_out_done  = xSemaphoreCreateBinary();
    s_ctrl_done = xSemaphoreCreateBinary();
    if (!s_out_done || !s_ctrl_done) {
        ESP_LOGE(TAG, "semaphore create failed");
        return false;
    }

    esp_log_level_set("HCD", ESP_LOG_VERBOSE);
    esp_log_level_set("USB_HOST", ESP_LOG_VERBOSE);
    esp_log_level_set("HUB", ESP_LOG_VERBOSE);
    esp_log_level_set("USBH", ESP_LOG_VERBOSE);

    /* ── Step 1: Explicitly configure the USB PHY for OTG Host mode ─── */
    usb_phy_config_t phy_cfg = {
        .controller = USB_PHY_CTRL_OTG,
        .target     = USB_PHY_TARGET_INT,   /* internal PHY on GPIO19/20 */
        .otg_mode   = USB_OTG_MODE_HOST,
        .otg_speed  = USB_PHY_SPEED_UNDEFINED, /* auto-detect */
    };
    esp_err_t ret = usb_new_phy(&phy_cfg, &s_phy_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "usb_new_phy failed: %s", esp_err_to_name(ret));
        return false;
    }
    ESP_LOGI(TAG, "usb_new_phy OK — internal PHY configured for OTG Host");

    /* ── Step 2: Install USB Host Library (PHY already set up) ──────── */
    usb_host_config_t host_cfg = {
        .skip_phy_setup = true,   /* we did it in step 1 */
        .intr_flags     = ESP_INTR_FLAG_LEVEL1,
    };
    ret = usb_host_install(&host_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(ret));
        return false;
    }
    ESP_LOGI(TAG, "usb_host_install OK (skip_phy_setup=true)");

    /* ── Diagnostic: dump HW registers ─────────────────────────────── */
    {
        uint32_t hprt = USB_DWC.hprt_reg.val;
        uint32_t wrap = USB_WRAP.otg_conf.val;
        ESP_LOGI(TAG, "DIAG: HPRT=0x%08lX conn=%d pwr=%d lineState=%d | "
                 "WRAP=0x%08lX pad_enable=%d phy_sel=%d",
                 (unsigned long)hprt,
                 (int)((hprt >> 0) & 1), (int)((hprt >> 12) & 1),
                 (int)((hprt >> 10) & 3),
                 (unsigned long)wrap,
                 (int)USB_WRAP.otg_conf.pad_enable,
                 (int)((wrap >> 2) & 1));
    }

    usb_host_client_config_t client_cfg = {
        .is_synchronous    = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg          = NULL,
        },
    };
    ret = usb_host_client_register(&client_cfg, &s_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_client_register failed: %s", esp_err_to_name(ret));
        usb_host_uninstall();
        return false;
    }

    xTaskCreate(usb_lib_task,      "usb_lib",      4096, NULL, 5, NULL);
    xTaskCreate(usb_client_task,   "usb_client",   4096, NULL, 5, NULL);
    xTaskCreate(usb_watchdog_task, "usb_watchdog", 2048, NULL, 2, NULL);

    ESP_LOGI(TAG, "USB Host stack started — waiting for device plug-in");
    return true;
}

static ssize_t usb_drv_recv(uint8_t *buf, size_t max_len)
{
    if (!s_dev || !s_in_xfer) return -1;
    
    usb_frame_t frame;
    /* Demand-driven polling for IN endpoints */
    if (usb_host_transfer_submit(s_in_xfer) != ESP_OK) {
        return -1;
    }
    
    /* Wait for the callback to push the result */
    if (xQueueReceive(s_rx_queue, &frame, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return -1;
    }
    /* If the callback returned the error frame length, it means STALL/NAK/Error */
    if (frame.len == 0xFFFF) return -1;
    
    size_t copy = (frame.len < max_len) ? frame.len : max_len;
    memcpy(buf, frame.data, copy);
    return (ssize_t)copy;
}

static bool usb_drv_send(const uint8_t *buf, size_t len)
{
    if (!s_out_xfer || !s_dev) {
        return true;   /* no OUT pipe — drop silently (normal for HID) */
    }
    if (len > (size_t)s_out_mps) len = (size_t)s_out_mps;
    memcpy(s_out_xfer->data_buffer, buf, len);
    s_out_xfer->num_bytes = len;
    return usb_host_transfer_submit(s_out_xfer) == ESP_OK;
}

const device_ops_t USB_DRIVER_OPS = {
    .init = usb_drv_init,
    .recv = usb_drv_recv,
    .send = usb_drv_send,
};

/* ── Public blocking / control API ───────────────────────────────────────── */

bool usb_drv_is_device_ready(void)
{
    return s_dev != NULL;
}

int usb_drv_send_blocking(const uint8_t *buf, size_t len)
{
    if (!s_dev) {
        ESP_LOGW(TAG, "usb_drv_send_blocking: no USB device connected");
        return 0;
    }
    if (!s_out_xfer) {
        ESP_LOGW(TAG, "usb_drv_send_blocking: no OUT pipe (device has no OUT endpoint)");
        return 0;   /* HID-only device — OUT not available */
    }
    if (len > (size_t)s_out_mps) len = (size_t)s_out_mps;
    memcpy(s_out_xfer->data_buffer, buf, len);
    s_out_xfer->num_bytes = len;
    if (usb_host_transfer_submit(s_out_xfer) != ESP_OK) return -1;
    if (xSemaphoreTake(s_out_done, pdMS_TO_TICKS(1000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    return s_out_status;  /* 0 = USB_TRANSFER_STATUS_COMPLETED */
}

int usb_drv_control(const uint8_t setup[8], const uint8_t *out_data, int out_len,
                    uint8_t *in_buf, int max_in)
{
    if (!s_dev) {
        ESP_LOGW(TAG, "usb_drv_control: no USB device connected");
        return -1;
    }
    if (!s_ctrl_xfer) {
        ESP_LOGW(TAG, "usb_drv_control: ctrl transfer not allocated yet");
        return -1;
    }

    /* Build setup + optional data in the transfer buffer.
     * usb_host requires: [8-byte setup][data] in one contiguous buffer. */
    memcpy(s_ctrl_xfer->data_buffer, setup, 8);
    if (out_data && out_len > 0) {
        if (out_len > 1024) {
            ESP_LOGE(TAG, "usb_drv_control: OUT data too large (%d > 1024)", out_len);
            return -1;
        }
        memcpy(s_ctrl_xfer->data_buffer + 8, out_data, (size_t)out_len);
    }

    size_t data_len = out_data ? (size_t)out_len : (size_t)(max_in > 0 ? max_in : 0);
    if (data_len > 1024) {
        ESP_LOGE(TAG, "usb_drv_control: stage data too large (%zu > 1024)", data_len);
        return -1;
    }
    s_ctrl_xfer->num_bytes = 8 + data_len;
    ESP_LOGI(TAG, "usb_drv_control: submitting transfer (num_bytes=%zu)", s_ctrl_xfer->num_bytes);
    if (usb_host_transfer_submit(s_ctrl_xfer) != ESP_OK) {
        ESP_LOGE(TAG, "usb_drv_control: submit failed");
        return -1;
    }

    if (xSemaphoreTake(s_ctrl_done, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGW(TAG, "usb_drv_control: ctrl transfer timed out (2 s)");
        return -1;   /* negative = error; avoids ESP_ERR_TIMEOUT (263) being
                        mistaken for a valid byte count by the caller */
    }
    ESP_LOGI(TAG, "usb_drv_control: done status=%d actual=%d",
             s_ctrl_status, s_ctrl_actual);
    if (s_ctrl_status != 0) return -(int)s_ctrl_status;

    /* Copy IN data out if caller wants it */
    if (in_buf && max_in > 0 && s_ctrl_actual > 0) {
        int copy = (s_ctrl_actual < max_in) ? s_ctrl_actual : max_in;
        memcpy(in_buf, s_ctrl_xfer->data_buffer + 8, (size_t)copy);
        return copy;
    }
    return s_ctrl_actual;
}
