#include "eth_driver.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <string.h>
#include <errno.h>

static const char *TAG = "EthDrv";

/* ============================================================================
 * MOCK DRIVER  (ETH_MOCK_DRIVER defined at compile time)
 * ============================================================================
 * Provides a minimal, hardware-free implementation for development and CI:
 *   init  – creates a FreeRTOS queue that stands in for the real RX ring
 *   recv  – blocks on the queue for up to 1 s; returns -1 on timeout
 *   send  – logs the frame and optionally loops it back into the RX queue
 *
 * To enable:  add `-DETH_MOCK_DRIVER` to compiler flags or CMakeLists.txt
 * ========================================================================== */
#ifdef ETH_MOCK_DRIVER

#define MOCK_FRAME_MAX  1520
#define MOCK_QUEUE_DEPTH  4

typedef struct {
    uint8_t  data[MOCK_FRAME_MAX];
    uint16_t len;
} mock_frame_t;

static QueueHandle_t s_mock_queue;

/* Periodic task: inject a fake ARP-who-has every ~5 s so the session
   has something to forward and the code path is exercised end-to-end. */
static void mock_inject_task(void *arg)
{
    (void)arg;
    /* Minimal broadcast ARP request (28-byte payload + 14-byte header = 42) */
    static const uint8_t fake_arp[] = {
        /* Ethernet header */
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,  /* dst: broadcast */
        0xDE,0xAD,0xBE,0xEF,0x00,0x01, /* src: fake MAC */
        0x08,0x06,                       /* EtherType: ARP */
        /* ARP */
        0x00,0x01, 0x08,0x00,           /* HTYPE=Ethernet, PTYPE=IPv4 */
        0x06, 0x04,                      /* HLEN=6, PLEN=4 */
        0x00,0x01,                       /* OPER=request */
        0xDE,0xAD,0xBE,0xEF,0x00,0x01, /* sender MAC */
        0x04,0x03,0x02,0x0A,            /* sender IP 4.3.2.10 */
        0x00,0x00,0x00,0x00,0x00,0x00, /* target MAC (unknown) */
        0x04,0x03,0x02,0x01,            /* target IP 4.3.2.1 */
    };

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        static mock_frame_t frame;
        memcpy(frame.data, fake_arp, sizeof(fake_arp));
        frame.len = sizeof(fake_arp);
        xQueueSend(s_mock_queue, &frame, 0);
        ESP_LOGD(TAG, "[MOCK] injected fake ARP (%u B)", frame.len);
    }
}

static bool mock_init(void)
{
    s_mock_queue = xQueueCreate(MOCK_QUEUE_DEPTH, sizeof(mock_frame_t));
    if (!s_mock_queue) {
        ESP_LOGE(TAG, "[MOCK] queue create failed");
        return false;
    }
    xTaskCreate(mock_inject_task, "eth_mock_inj", 2048, NULL, 3, NULL);
    ESP_LOGI(TAG, "[MOCK] Ethernet mock driver ready");
    return true;
}

static ssize_t mock_recv(uint8_t *buf, size_t max_len)
{
    static mock_frame_t frame;
    if (xQueueReceive(s_mock_queue, &frame, pdMS_TO_TICKS(1000)) == pdFALSE) {
        return -1;   /* timeout */
    }
    size_t copy = (frame.len < max_len) ? frame.len : max_len;
    memcpy(buf, frame.data, copy);
    return (ssize_t)copy;
}

static bool mock_send(const uint8_t *buf, size_t len)
{
    ESP_LOGI(TAG, "[MOCK] daemon→ESP32 frame received: %d B (no real NIC, dropped)", (int)len);
    (void)buf;
    return true;
}

const device_ops_t ETH_DRIVER_OPS = {
    .init = mock_init,
    .recv = mock_recv,
    .send = mock_send,
};

/* ============================================================================
 * REAL W5500 DRIVER  (default)
 * ============================================================================
 * Uses the ESP-IDF esp_eth + W5500 SPI MAC components.
 *
 * Received frames are delivered via an esp_eth handle callback into an
 * internal FreeRTOS queue; send() calls esp_eth_transmit() directly.
 *
 * Requires PRIV_REQUIRES: esp_eth, driver (spi_master) in CMakeLists.txt.
 * ========================================================================== */
#else /* !ETH_MOCK_DRIVER */

#include "esp_eth.h"
#include "esp_eth_mac_spi.h"
#include "driver/spi_master.h"

#define REAL_FRAME_MAX   1520
#define REAL_QUEUE_DEPTH 8

typedef struct {
    uint8_t  data[REAL_FRAME_MAX];
    uint16_t len;
} real_frame_t;

static QueueHandle_t   s_rx_queue;
static esp_eth_handle_t s_eth_handle;

static esp_err_t eth_input_cb(esp_eth_handle_t hdl,
                               uint8_t *buf, uint32_t len,
                               void *priv)
{
    (void)hdl; (void)priv;
    if (len > REAL_FRAME_MAX) {
        ESP_LOGW(TAG, "frame too large (%lu B), dropped", (unsigned long)len);
        free(buf);
        return ESP_OK;
    }
    real_frame_t frame;
    memcpy(frame.data, buf, len);
    frame.len = (uint16_t)len;
    free(buf);   /* esp_eth hands us a malloc'd buffer */

    if (xQueueSend(s_rx_queue, &frame, 0) != pdTRUE) {
        ESP_LOGW(TAG, "RX queue full, frame dropped");
    }
    return ESP_OK;
}

static bool real_init(void)
{
    s_rx_queue = xQueueCreate(REAL_QUEUE_DEPTH, sizeof(real_frame_t));
    if (!s_rx_queue) {
        ESP_LOGE(TAG, "failed to create RX queue");
        return false;
    }

    /* ── SPI bus ── */
    spi_bus_config_t buscfg = {
        .mosi_io_num   = ETH_PIN_MOSI,
        .miso_io_num   = ETH_PIN_MISO,
        .sclk_io_num   = ETH_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(ETH_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    /* ── W5500 MAC (SPI) ── */
    spi_device_interface_config_t devcfg = {
        .command_bits     = 16,
        .address_bits     = 8,
        .mode             = 0,
        .clock_speed_hz   = ETH_SPI_CLOCK_MHZ * 1000 * 1000,
        .spics_io_num     = ETH_PIN_CS,
        .queue_size       = 20,
    };
    eth_w5500_config_t w5500_cfg = ETH_W5500_DEFAULT_CONFIG(ETH_SPI_HOST, &devcfg);
    w5500_cfg.int_gpio_num = ETH_PIN_INT;

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t   *mac     = esp_eth_mac_new_w5500(&w5500_cfg, &mac_cfg);
    if (!mac) {
        ESP_LOGE(TAG, "esp_eth_mac_new_w5500 failed");
        return false;
    }

    /* ── PHY (internal to W5500) ── */
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.reset_gpio_num   = ETH_PIN_RST;
    esp_eth_phy_t *phy       = esp_eth_phy_new_w5500(&phy_cfg);
    if (!phy) {
        ESP_LOGE(TAG, "esp_eth_phy_new_w5500 failed");
        return false;
    }

    /* ── Install driver ── */
    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_cfg, &s_eth_handle));
    ESP_ERROR_CHECK(esp_eth_update_input_path(s_eth_handle, eth_input_cb, NULL));
    ESP_ERROR_CHECK(esp_eth_start(s_eth_handle));

    ESP_LOGI(TAG, "W5500 SPI Ethernet started");
    return true;
}

static ssize_t real_recv(uint8_t *buf, size_t max_len)
{
    real_frame_t frame;
    if (xQueueReceive(s_rx_queue, &frame, pdMS_TO_TICKS(1000)) == pdFALSE) {
        return -1;
    }
    size_t copy = (frame.len < max_len) ? frame.len : max_len;
    memcpy(buf, frame.data, copy);
    return (ssize_t)copy;
}

static bool real_send(const uint8_t *buf, size_t len)
{
    /* esp_eth_transmit expects a mutable buffer; copy to avoid cast */
    uint8_t *tmp = malloc(len);
    if (!tmp) return false;
    memcpy(tmp, buf, len);
    esp_err_t err = esp_eth_transmit(s_eth_handle, tmp, len);
    free(tmp);
    return err == ESP_OK;
}

const device_ops_t ETH_DRIVER_OPS = {
    .init = real_init,
    .recv = real_recv,
    .send = real_send,
};

#endif /* ETH_MOCK_DRIVER */
