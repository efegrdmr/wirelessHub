#include "log_forwarder.h"
#include "hub_client.h"
#include "crash_reporter.h"
#include "Protocol.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include <sys/socket.h>
#include <netinet/in.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

/* Maximum forwarded message length. Typical ESP_LOG line is <200 B. */
#define LOG_MSG_MAX   256
/* Queue depth — lines are dropped (not blocked) when full. */
#define LOG_QUEUE_LEN 256

/* ── Internal state ──────────────────────────────────────────────────────── */
static int           s_sock  = -1;
static bool          s_connected = false;
static QueueHandle_t s_queue = NULL;

/* Entry stored in the queue (pointer, not value — avoids large stack copy). */
typedef struct {
    uint8_t  level;
    uint16_t len;
    char     msg[LOG_MSG_MAX];
} log_entry_t;

/* ── Log level detection ─────────────────────────────────────────────────── */
static uint8_t detect_level(const char *s)
{
    /* Skip ANSI CSI sequences: ESC [ <params> m */
    while (*s == '\x1B') {
        s++;
        if (*s == '[') { s++; while (*s && *s != 'm') s++; if (*s == 'm') s++; }
    }
    switch (*s) {
        case 'E': return LOG_LEVEL_ERROR;
        case 'W': return LOG_LEVEL_WARN;
        case 'I': return LOG_LEVEL_INFO;
        case 'D': return LOG_LEVEL_DEBUG;
        case 'V': return LOG_LEVEL_VERBOSE;
        default:  return LOG_LEVEL_INFO;
    }
}

/* ── vprintf hook ────────────────────────────────────────────────────────── */
/*
 * DESIGN RULE: this hook MUST NOT call sendto() or any blocking lwIP call.
 * WiFi-internal tasks (ppTask, wifi_driver, etc.) emit ESP_LOG lines through
 * this hook while holding WiFi/lwIP locks. Calling sendto() from here would
 * immediately deadlock those tasks.
 *
 * Instead: render the string, enqueue a pointer, return.  A separate
 * log_sender_task() owns the sendto() call.
 */
static int log_forwarder_vprintf(const char *fmt, va_list args)
{
    /* Print to serial first (va_copy so original args remain intact). */
    va_list args_serial;
    va_copy(args_serial, args);
    int ret = vprintf(fmt, args_serial);
    va_end(args_serial);

    /* Append to RTC ring buffer for crash post-mortem (always, no alloc). */
    {
        va_list args_rtc;
        va_copy(args_rtc, args);
        char rtc_tmp[128];
        int rn = vsnprintf(rtc_tmp, sizeof(rtc_tmp), fmt, args_rtc);
        va_end(args_rtc);
        if (rn > 0) crash_reporter_append(rtc_tmp, (size_t)(rn < 128 ? rn : 127));
    }

    if (!s_queue)
        return ret;

    /* Malloc the entry so the hook stack stays small (~16 B local vars). */
    log_entry_t *e = malloc(sizeof(log_entry_t));
    if (!e) return ret;

    int n = vsnprintf(e->msg, LOG_MSG_MAX, fmt, args);
    if (n <= 0) { free(e); return ret; }
    e->len   = (uint16_t)(n < LOG_MSG_MAX ? n : LOG_MSG_MAX - 1);
    e->level = detect_level(e->msg);

    /* Non-blocking send — drop the entry rather than block. */
    if (xQueueSend(s_queue, &e, 0) != pdTRUE)
        free(e);

    return ret;
}

/* ── Sender task ─────────────────────────────────────────────────────────── */
static void log_sender_task(void *arg)
{
    uint8_t pkt[sizeof(Header) + sizeof(LogPayload) + LOG_MSG_MAX];
    int reconnect_delay_ms = 500; // Start with 0.5s, max 10s

    while (true) {
        if (!hub_client_daemon_found()) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        log_entry_t *e = NULL;
        if (xQueueReceive(s_queue, &e, portMAX_DELAY) != pdTRUE || !e)
            continue;

        // Ensure TCP connection is established
        while (s_sock < 0 || !s_connected) {
            if (s_sock >= 0) close(s_sock);
            s_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
            if (s_sock < 0) {
                s_connected = false;
                printf("[log_forwarder] TCP socket() failed: errno %d\n", errno);
                vTaskDelay(pdMS_TO_TICKS(reconnect_delay_ms));
                reconnect_delay_ms = (reconnect_delay_ms < 10000) ? reconnect_delay_ms * 2 : 10000;
                continue;
            }
            struct sockaddr_in dest;
            memset(&dest, 0, sizeof(dest));
            dest.sin_family      = AF_INET;
            dest.sin_addr.s_addr = hub_client_daemon_ip();
            dest.sin_port        = htons(hub_client_daemon_port());
            if (connect(s_sock, (struct sockaddr *)&dest, sizeof(dest)) < 0) {
                printf("[log_forwarder] TCP connect() failed: errno %d\n", errno);
                close(s_sock);
                s_sock = -1;
                s_connected = false;
                vTaskDelay(pdMS_TO_TICKS(reconnect_delay_ms));
                reconnect_delay_ms = (reconnect_delay_ms < 10000) ? reconnect_delay_ms * 2 : 10000;
                continue;
            }
            s_connected = true;
            reconnect_delay_ms = 500; // Reset delay on success
            printf("[log_forwarder] TCP log connection established\n");
        }

        size_t pay_len  = sizeof(LogPayload) + e->len;
        size_t pkt_size = sizeof(Header) + pay_len;

        Header hdr;
        memset(&hdr, 0, sizeof(hdr));
        hdr.cmd_type    = CMD_LOG;
        hdr.device_id   = 0x00;
        hdr.payload_len = (uint16_t)pay_len;

        LogPayload lp = { .log_level = e->level };

        size_t off = 0;
        memcpy(pkt + off, &hdr,    sizeof(hdr));  off += sizeof(hdr);
        memcpy(pkt + off, &lp,     sizeof(lp));   off += sizeof(lp);
        memcpy(pkt + off, e->msg,  e->len);

        free(e);   /* release before send to minimise hold time */

        ssize_t sent = send(s_sock, pkt, pkt_size, 0);
        if (sent < 0) {
            printf("[log_forwarder] TCP send() failed: errno %d\n", errno);
            close(s_sock);
            s_sock = -1;
            s_connected = false;
            vTaskDelay(pdMS_TO_TICKS(reconnect_delay_ms));
            reconnect_delay_ms = (reconnect_delay_ms < 10000) ? reconnect_delay_ms * 2 : 10000;
        }
    }
}

/* ── Public init ─────────────────────────────────────────────────────────── */
void log_forwarder_init(void)
{
    s_sock = -1;
    s_connected = false;

    /* Queue stores pointers, not the structs themselves. */
    s_queue = xQueueCreate(LOG_QUEUE_LEN, sizeof(log_entry_t *));
    if (!s_queue) {
        printf("[log_forwarder] xQueueCreate failed\n");
        return;
    }

    xTaskCreate(log_sender_task, "log_sender", 4096, NULL, 3, NULL);

    esp_log_set_vprintf(log_forwarder_vprintf);
}
