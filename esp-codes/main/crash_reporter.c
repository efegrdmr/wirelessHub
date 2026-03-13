#include "crash_reporter.h"
#include "hub_client.h"

#include "esp_attr.h"
#include "esp_system.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdio.h>
#include <stdbool.h>

static const char *TAG = "CrashRpt";

#define CRASH_MAGIC   0xC0FFEE42u
#define RTC_BUF_SIZE  600   /* keep small — RTC slow RAM is shared (~8 KB) */

/* ── RTC RAM: survives soft-reset / panic ──────────────────────────────── */
static RTC_DATA_ATTR uint32_t s_magic;
static RTC_DATA_ATTR char     s_buf[RTC_BUF_SIZE];
static RTC_DATA_ATTR uint16_t s_pos;
static RTC_DATA_ATTR uint8_t  s_wrapped;

/* ── Normal RAM: populated once at boot, then sent when daemon appears ─── */
static char    s_pending[RTC_BUF_SIZE + 64];
static bool    s_has_pending  = false;
static uint8_t s_pending_reason = 0;

/* ── Append to RTC ring (called from log hook — no alloc, no RTOS) ─────── */
void crash_reporter_append(const char *msg, size_t len)
{
    if (s_magic != CRASH_MAGIC) return;
    for (size_t i = 0; i < len; i++) {
        s_buf[s_pos] = msg[i];
        if (++s_pos >= RTC_BUF_SIZE) { s_pos = 0; s_wrapped = 1; }
    }
}

/* ── Sender task: waits for daemon, then emits the saved crash report ──── */
static void crash_send_task(void *arg)
{
    (void)arg;
    xEventGroupWaitBits(hub_client_event_group(), DAEMON_FOUND_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(1500));   /* let UDP socket settle */

    const char *reason_str;
    switch ((esp_reset_reason_t)s_pending_reason) {
        case ESP_RST_PANIC:    reason_str = "PANIC";    break;
        case ESP_RST_TASK_WDT: reason_str = "TASK_WDT"; break;
        case ESP_RST_INT_WDT:  reason_str = "INT_WDT";  break;
        case ESP_RST_WDT:      reason_str = "WDT";      break;
        case ESP_RST_SW:       reason_str = "SW_RESET"; break;
        case ESP_RST_BROWNOUT: reason_str = "BROWNOUT"; break;
        default:               reason_str = "UNKNOWN";  break;
    }

    size_t total = strlen(s_pending);
    ESP_LOGE(TAG, "====== PREVIOUS CRASH: %s (code=%d) log_bytes=%d ======",
             reason_str, s_pending_reason, (int)total);

    if (total > 0) {
        /* Emit in 200-byte chunks so log_forwarder doesn't truncate */
        for (size_t off = 0; off < total; off += 200) {
            size_t chunk = (total - off < 200) ? (total - off) : 200;
            ESP_LOGE(TAG, "%.*s", (int)chunk, s_pending + off);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    } else {
        ESP_LOGE(TAG, "(no log captured before crash)");
    }

    ESP_LOGE(TAG, "====== END CRASH REPORT ======");
    vTaskDelete(NULL);
}

/* ── Init: call at the very top of app_main ────────────────────────────── */
void crash_reporter_init(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    bool is_crash = (reason == ESP_RST_PANIC    ||
                     reason == ESP_RST_TASK_WDT  ||
                     reason == ESP_RST_INT_WDT   ||
                     reason == ESP_RST_WDT        ||
                     reason == ESP_RST_BROWNOUT   ||
                     reason == ESP_RST_SW);

    if (is_crash && s_magic == CRASH_MAGIC && (s_pos > 0 || s_wrapped)) {
        s_has_pending    = true;
        s_pending_reason = (uint8_t)reason;

        /* Reconstruct ring buffer in chronological order */
        size_t out = 0;
        if (s_wrapped) {
            size_t tail = RTC_BUF_SIZE - s_pos;
            memcpy(s_pending + out, s_buf + s_pos, tail); out += tail;
            memcpy(s_pending + out, s_buf,         s_pos); out += s_pos;
        } else {
            memcpy(s_pending, s_buf, s_pos); out = s_pos;
        }
        s_pending[out] = '\0';

        /* Sanitise non-printable chars */
        for (size_t i = 0; i < out; i++) {
            char c = s_pending[i];
            if (c != '\n' && c != '\t' && (c < 0x20 || (uint8_t)c > 0x7E))
                s_pending[i] = '.';
        }
    }

    /* Re-initialise RTC buffer for this session */
    s_magic   = CRASH_MAGIC;
    s_pos     = 0;
    s_wrapped = 0;
    memset(s_buf, 0, sizeof(s_buf));

    if (s_has_pending) {
        ESP_LOGW(TAG, "Previous crash detected (reason=%d) — will report when daemon connects",
                 (int)reason);
        xTaskCreate(crash_send_task, "crash_send", 3072, NULL, 3, NULL);
    } else {
        ESP_LOGI(TAG, "Clean boot (reset_reason=%d)", (int)reason);
    }
}
