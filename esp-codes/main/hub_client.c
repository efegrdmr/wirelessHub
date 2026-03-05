#include "hub_client.h"
#include "wifi_manager.h"
#include "Protocol.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static const char *TAG = "HubClient";

/* ── Internal state ──────────────────────────────────────────────────────── */
typedef struct {
    bool     found;
    uint32_t ip;    /* network byte order  */
    uint16_t port;  /* host byte order     */
} hub_state_t;

static hub_state_t s_hub = { .found = false, .ip = 0, .port = 0 };

static EventGroupHandle_t s_hub_event_group = NULL;

/* ── Public accessors ──────────────────────────────────────────────────────────────── */
EventGroupHandle_t hub_client_event_group(void) { return s_hub_event_group; }
bool     hub_client_daemon_found(void) { return s_hub.found; }
uint32_t hub_client_daemon_ip(void)    { return s_hub.ip;    }
uint16_t hub_client_daemon_port(void)  { return s_hub.port;  }

/* ── Discovery task ──────────────────────────────────────────────────────── */
static void hub_discover_task(void *arg)
{
    EventGroupHandle_t wifi_eg = wifi_manager_event_group();

    /* Outer loop: re-runs every time WiFi is (re)established. */
    while (true) {

        /* ── Wait for network ────────────────────────────────────────── */
        xEventGroupWaitBits(wifi_eg, WIFI_READY_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
        ESP_LOGI(TAG, "WiFi ready — starting daemon discovery");

        /* ── Inner discovery loop ────────────────────────────────────── */
        int  attempt = 0;
        bool found   = false;
        while (!found) {
            attempt++;
            ESP_LOGI(TAG, "DISCOVER attempt %d", attempt);

            /* Open UDP socket */
            int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (sock < 0) {
                ESP_LOGE(TAG, "socket() failed: errno %d", errno);
                vTaskDelay(pdMS_TO_TICKS(DISCOVER_RETRY_SEC * 1000));
                continue;
            }

            /* Enable broadcast */
            int broadcast = 1;
            if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST,
                           &broadcast, sizeof(broadcast)) < 0) {
                ESP_LOGE(TAG, "SO_BROADCAST failed: errno %d", errno);
                close(sock);
                vTaskDelay(pdMS_TO_TICKS(DISCOVER_RETRY_SEC * 1000));
                continue;
            }

            /* Receive timeout */
            struct timeval tv = { .tv_sec = DISCOVER_TIMEOUT_SEC, .tv_usec = 0 };
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            /* Build and send DISCOVER packet */
            Header pkt;
            memset(&pkt, 0, sizeof(pkt));
            pkt.cmd_type    = CMD_DISCOVER;
            pkt.payload_len = 0;

            struct sockaddr_in dest;
            memset(&dest, 0, sizeof(dest));
            dest.sin_family      = AF_INET;
            dest.sin_port        = htons(DAEMON_PORT);
            dest.sin_addr.s_addr = htonl(INADDR_BROADCAST);

            ssize_t sent = sendto(sock, &pkt, sizeof(pkt), 0,
                                  (struct sockaddr *)&dest, sizeof(dest));
            if (sent < 0) {
                ESP_LOGE(TAG, "sendto() failed: errno %d", errno);
                close(sock);
                vTaskDelay(pdMS_TO_TICKS(DISCOVER_RETRY_SEC * 1000));
                continue;
            }
            ESP_LOGI(TAG, "DISCOVER sent (%d bytes) → 255.255.255.255:%u",
                     (int)sent, (unsigned)DAEMON_PORT);

            /* Wait for DISCOVER_REPLY */
            uint8_t buf[sizeof(Header) + sizeof(DiscoverReplyPayload)];
            struct sockaddr_in sender;
            socklen_t sender_len = sizeof(sender);

            ssize_t rcvd = recvfrom(sock, buf, sizeof(buf), 0,
                                    (struct sockaddr *)&sender, &sender_len);
            close(sock);

            if (rcvd < (ssize_t)(sizeof(Header) + sizeof(DiscoverReplyPayload))) {
                ESP_LOGW(TAG, "No valid reply (rcvd=%d) — retrying in %d s...",
                         (int)rcvd, DISCOVER_RETRY_SEC);
                vTaskDelay(pdMS_TO_TICKS(DISCOVER_RETRY_SEC * 1000));
                continue;
            }

            Header *hdr = (Header *)buf;
            if (hdr->cmd_type != CMD_DISCOVER_REPLY) {
                ESP_LOGW(TAG, "Unexpected cmd_type=0x%02x — retrying in %d s...",
                         hdr->cmd_type, DISCOVER_RETRY_SEC);
                vTaskDelay(pdMS_TO_TICKS(DISCOVER_RETRY_SEC * 1000));
                continue;
            }

            /* ── Success ─────────────────────────────────────────────── */
            DiscoverReplyPayload *reply =
                (DiscoverReplyPayload *)(buf + sizeof(Header));

            s_hub.found = true;
            s_hub.ip    = sender.sin_addr.s_addr;   /* network byte order */
            s_hub.port  = reply->daemon_port;        /* little-endian == host on ESP32 */

            char ip_str[16];
            inet_ntoa_r(sender.sin_addr, ip_str, sizeof(ip_str));
            ESP_LOGI(TAG, "Daemon found at %s:%u ✓", ip_str, (unsigned)s_hub.port);

            xEventGroupClearBits(s_hub_event_group, DAEMON_LOST_BIT);
            xEventGroupSetBits(s_hub_event_group, DAEMON_FOUND_BIT);
            found = true;
        }

        /* ── Wait for WiFi to drop ───────────────────────────────────── */
        xEventGroupWaitBits(wifi_eg, WIFI_LOST_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
        ESP_LOGW(TAG, "WiFi lost — clearing daemon state, will rediscover on reconnect");

        s_hub.found = false;
        s_hub.ip    = 0;
        s_hub.port  = 0;
        xEventGroupClearBits(s_hub_event_group, DAEMON_FOUND_BIT);
        xEventGroupSetBits(s_hub_event_group, DAEMON_LOST_BIT);
    }
}

/* ── Public entry point ──────────────────────────────────────────────────── */
void hub_client_start(void)
{
    s_hub_event_group = xEventGroupCreate();
    xTaskCreate(hub_discover_task, "hub_discover", 4096, NULL, 5, NULL);
}
