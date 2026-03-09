#include "device_session.h"
#include "hub_client.h"
#include "Protocol.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>

static const char *TAG = "DevSess";

// ── Reassembly ────────────────────────────────────────────────────────────────
// One-slot reassembly: handles a single in-flight fragmented transfer at a time.
// For Ethernet (max 1518 B / 1400 B per frag = 2 frags) this is always enough.
// For large USB bulk transfers this would need to be extended.
#define REASM_MAX_FRAGS  8
#define REASM_FRAG_LEN   MTU_PAYLOAD

typedef struct {
    bool     active;
    uint16_t transfer_seq;
    uint8_t  total;
    uint8_t  received;
    bool     have[REASM_MAX_FRAGS];
    uint8_t  data[REASM_MAX_FRAGS][REASM_FRAG_LEN];
    uint16_t lens[REASM_MAX_FRAGS];
} reasm_state_t;

// ── Per-session state ─────────────────────────────────────────────────────────
#define SESSION_MAX_ID  5               /* device_id 0x00..0x04 */

typedef struct {
    const device_session_cfg_t *cfg;
    EventGroupHandle_t          eg;
    volatile int                sock;   /* reply UDP socket; -1 = inactive */
    uint32_t                    daemon_ip;
    uint8_t                     seq;
    uint16_t                    out_transfer_seq;
    reasm_state_t               reasm;
} session_t;

static session_t s_sessions[SESSION_MAX_ID];

// Per-session task buffers (static — not on stack)
static uint8_t s_tx_buf[SESSION_MAX_ID][1600];
static uint8_t s_rx_buf[SESSION_MAX_ID][sizeof(Header) + sizeof(FragHeader) + MTU_PAYLOAD + 8];

// ── Helpers ───────────────────────────────────────────────────────────────────

static void send_device_event(session_t *s, uint8_t event)
{
    uint16_t reply_port = DEVICE_BASE_PORT + s->cfg->device_id;

    uint8_t pkt[sizeof(Header) + sizeof(DeviceEventPayload)];
    Header hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.cmd_type    = CMD_DEVICE_EVENT;
    hdr.device_id   = s->cfg->device_id;
    hdr.seq         = s->seq++;
    hdr.payload_len = sizeof(DeviceEventPayload);

    DeviceEventPayload dep;
    memset(&dep, 0, sizeof(dep));
    dep.device_id  = s->cfg->device_id;
    dep.event      = event;
    dep.speed      = s->cfg->speed;
    dep.usb_class  = s->cfg->usb_class;
    dep.subclass   = s->cfg->subclass;
    dep.protocol   = s->cfg->protocol;
    dep.reply_port = reply_port;

    memcpy(pkt,               &hdr, sizeof(hdr));
    memcpy(pkt + sizeof(hdr), &dep, sizeof(dep));

    /* Open a temporary socket for this one packet */
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "[dev=%02X] socket() failed for DEVICE_EVENT: errno %d",
                 s->cfg->device_id, errno);
        return;
    }
    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family      = AF_INET;
    dest.sin_port        = htons(DAEMON_PORT);
    dest.sin_addr.s_addr = s->daemon_ip;
    sendto(sock, pkt, sizeof(pkt), 0, (struct sockaddr *)&dest, sizeof(dest));
    close(sock);

    ESP_LOGI(TAG, "[dev=%02X] CMD_DEVICE_EVENT %s sent → daemon reply_port=%u",
             s->cfg->device_id,
             (event == DEVICE_EVENT_CONNECT ? "CONNECT" : "DISCONNECT"),
             reply_port);
}

static void send_raw(session_t *s, const uint8_t *data, size_t len)
{
    int sock = s->sock;
    if (sock < 0) return;

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family      = AF_INET;
    dest.sin_port        = htons(DAEMON_PORT);
    dest.sin_addr.s_addr = s->daemon_ip;

    if (len <= MTU_PAYLOAD) {
        /* CMD_RAW_DATA — single datagram */
        uint8_t pkt[sizeof(Header) + MTU_PAYLOAD];
        Header hdr;
        memset(&hdr, 0, sizeof(hdr));
        hdr.cmd_type    = CMD_RAW_DATA;
        hdr.device_id   = s->cfg->device_id;
        hdr.seq         = s->seq++;
        hdr.payload_len = (uint16_t)len;
        memcpy(pkt,               &hdr, sizeof(hdr));
        memcpy(pkt + sizeof(hdr), data, len);
        sendto(sock, pkt, sizeof(hdr) + len, 0,
               (struct sockaddr *)&dest, sizeof(dest));
    } else {
        /* CMD_RAW_FRAG — multiple datagrams */
        size_t   total_frags = (len + MTU_PAYLOAD - 1) / MTU_PAYLOAD;
        uint16_t tseq        = s->out_transfer_seq++;

        if (total_frags > 255) {
            ESP_LOGE(TAG, "[dev=%02X] frame too large (%d B), dropping",
                     s->cfg->device_id, (int)len);
            return;
        }

        uint8_t pkt[sizeof(Header) + sizeof(FragHeader) + MTU_PAYLOAD];
        for (size_t i = 0; i < total_frags; i++) {
            size_t offset    = i * MTU_PAYLOAD;
            size_t chunk_len = ((offset + MTU_PAYLOAD) <= len)
                               ? MTU_PAYLOAD : (len - offset);

            Header hdr;
            memset(&hdr, 0, sizeof(hdr));
            hdr.cmd_type    = CMD_RAW_FRAG;
            hdr.device_id   = s->cfg->device_id;
            hdr.seq         = s->seq++;
            hdr.payload_len = (uint16_t)(sizeof(FragHeader) + chunk_len);

            FragHeader fhdr;
            fhdr.transfer_seq = tseq;
            fhdr.frag_idx     = (uint8_t)i;
            fhdr.frag_total   = (uint8_t)total_frags;

            size_t off = 0;
            memcpy(pkt + off, &hdr,  sizeof(hdr));  off += sizeof(hdr);
            memcpy(pkt + off, &fhdr, sizeof(fhdr)); off += sizeof(fhdr);
            memcpy(pkt + off, data + offset, chunk_len);

            sendto(sock, pkt, off + chunk_len, 0,
                   (struct sockaddr *)&dest, sizeof(dest));
        }
    }
}

/* Returns true and writes assembled frame to out_buf if transfer is complete. */
static bool reasm_push(reasm_state_t *r, const device_ops_t *ops,
                       uint16_t tseq, uint8_t idx, uint8_t total,
                       const uint8_t *chunk, uint16_t chunk_len)
{
    if (total == 0 || idx >= total || idx >= REASM_MAX_FRAGS) return false;
    if (chunk_len > REASM_FRAG_LEN) chunk_len = REASM_FRAG_LEN;

    /* New transfer or transfer_seq mismatch — reset */
    if (!r->active || r->transfer_seq != tseq) {
        memset(r, 0, sizeof(*r));
        r->active       = true;
        r->transfer_seq = tseq;
        r->total        = total;
    }

    if (!r->have[idx]) {
        memcpy(r->data[idx], chunk, chunk_len);
        r->lens[idx] = chunk_len;
        r->have[idx] = true;
        r->received++;
    }

    if (r->received < r->total) return false;

    /* Reassemble and push to driver */
    for (uint8_t i = 0; i < r->total; i++) {
        if (!ops->send(r->data[i], r->lens[i])) break;
    }

    r->active = false;
    return true;
}

// ── Tasks ─────────────────────────────────────────────────────────────────────

static void session_ctrl_task(void *arg)
{
    session_t         *s      = (session_t *)arg;
    EventGroupHandle_t hub_eg = hub_client_event_group();

    while (true) {
        /* ── Wait for daemon ──────────────────────────────────────────── */
        xEventGroupWaitBits(hub_eg, DAEMON_FOUND_BIT,
                            pdFALSE, pdTRUE, portMAX_DELAY);

        s->daemon_ip = hub_client_daemon_ip();
        ESP_LOGI(TAG, "[dev=%02X] daemon found — opening reply socket",
                 s->cfg->device_id);

        /* ── Open reply socket ────────────────────────────────────────── */
        uint16_t reply_port = DEVICE_BASE_PORT + s->cfg->device_id;
        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock < 0) {
            ESP_LOGE(TAG, "[dev=%02X] socket() failed: errno %d",
                     s->cfg->device_id, errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        struct sockaddr_in bind_addr;
        memset(&bind_addr, 0, sizeof(bind_addr));
        bind_addr.sin_family      = AF_INET;
        bind_addr.sin_port        = htons(reply_port);
        bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
            ESP_LOGE(TAG, "[dev=%02X] bind(:%u) failed: errno %d",
                     s->cfg->device_id, reply_port, errno);
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        /* 1-second recv timeout so down_task can check SESSION_LOST_BIT */
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        /* ── Activate session ─────────────────────────────────────────── */
        s->sock = sock;
        memset(&s->reasm, 0, sizeof(s->reasm));
        s->seq              = 0;
        s->out_transfer_seq = 0;

        send_device_event(s, DEVICE_EVENT_CONNECT);

        xEventGroupClearBits(s->eg, SESSION_LOST_BIT);
        xEventGroupSetBits(s->eg,   SESSION_ACTIVE_BIT);
        ESP_LOGI(TAG, "[dev=%02X] session active (reply port %u)",
                 s->cfg->device_id, reply_port);

        /* ── Wait for daemon to be lost ───────────────────────────────── */
        xEventGroupWaitBits(hub_eg, DAEMON_LOST_BIT,
                            pdFALSE, pdTRUE, portMAX_DELAY);

        /* ── Tear down ────────────────────────────────────────────────── */
        ESP_LOGW(TAG, "[dev=%02X] daemon lost — tearing down session",
                 s->cfg->device_id);

        xEventGroupClearBits(s->eg, SESSION_ACTIVE_BIT);
        xEventGroupSetBits(s->eg,   SESSION_LOST_BIT);

        int old_sock = s->sock;
        s->sock = -1;
        close(old_sock);   /* unblocks down_task's recvfrom() */

        /* Give tx/rx tasks time to notice SESSION_LOST_BIT */
        vTaskDelay(pdMS_TO_TICKS(1500));

        send_device_event(s, DEVICE_EVENT_DISCONNECT);
        xEventGroupClearBits(s->eg, SESSION_LOST_BIT);
    }
}

static void session_up_task(void *arg)
{
    session_t *s   = (session_t *)arg;
    uint8_t   *buf = s_tx_buf[s->cfg->device_id];

    while (true) {
        /* ── Wait for active session ──────────────────────────────────── */
        xEventGroupWaitBits(s->eg, SESSION_ACTIVE_BIT,
                            pdFALSE, pdTRUE, portMAX_DELAY);

        /* ── Forward device frames to daemon ─────────────────────────── */
        while (true) {
            if (xEventGroupGetBits(s->eg) & SESSION_LOST_BIT) break;

            ssize_t n = s->cfg->ops->recv(buf, 1600);
            if (n <= 0) continue;   /* timeout — re-check bits */

            if (s->sock < 0) break;
            send_raw(s, buf, (size_t)n);
        }
    }
}

static void session_down_task(void *arg)
{
    session_t *s   = (session_t *)arg;
    uint8_t   *buf = s_rx_buf[s->cfg->device_id];
    size_t buf_len = sizeof(s_rx_buf[0]);

    while (true) {
        /* ── Wait for active session ──────────────────────────────────── */
        xEventGroupWaitBits(s->eg, SESSION_ACTIVE_BIT,
                            pdFALSE, pdTRUE, portMAX_DELAY);

        /* ── Forward daemon packets to device ────────────────────────── */
        while (true) {
            int sock = s->sock;
            if (sock < 0) break;

            struct sockaddr_in sender;
            socklen_t slen = sizeof(sender);
            ssize_t n = recvfrom(sock, buf, buf_len, 0,
                                 (struct sockaddr *)&sender, &slen);
            if (n < 0) {
                /* EAGAIN = 1-s timeout expired */
                if (xEventGroupGetBits(s->eg) & SESSION_LOST_BIT) break;
                continue;
            }
            if (n < (ssize_t)sizeof(Header)) continue;

            Header *hdr     = (Header *)buf;
            uint8_t *payload = buf + sizeof(Header);
            size_t pay_len   = (size_t)n - sizeof(Header);

            if (hdr->cmd_type == CMD_RAW_DATA) {
                s->cfg->ops->send(payload, pay_len);

            } else if (hdr->cmd_type == CMD_RAW_FRAG) {
                if (pay_len < sizeof(FragHeader)) continue;
                FragHeader *fhdr = (FragHeader *)payload;
                const uint8_t *chunk     = payload  + sizeof(FragHeader);
                uint16_t       chunk_len = (uint16_t)(pay_len - sizeof(FragHeader));
                reasm_push(&s->reasm, s->cfg->ops,
                           fhdr->transfer_seq, fhdr->frag_idx, fhdr->frag_total,
                           chunk, chunk_len);
            }
        }
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

void device_session_start(const device_session_cfg_t *cfg)
{
    if (cfg->device_id >= SESSION_MAX_ID) {
        ESP_LOGE(TAG, "device_id=0x%02X out of range (max %d)",
                 cfg->device_id, SESSION_MAX_ID - 1);
        return;
    }

    session_t *s = &s_sessions[cfg->device_id];
    if (s->eg != NULL) {
        ESP_LOGE(TAG, "device_id=0x%02X already started", cfg->device_id);
        return;
    }

    s->cfg  = cfg;
    s->sock = -1;
    s->eg   = xEventGroupCreate();

    if (!cfg->ops->init()) {
        ESP_LOGE(TAG, "[dev=%02X] ops->init() failed", cfg->device_id);
        return;
    }

    char name[24];

    snprintf(name, sizeof(name), "sess_ctrl_%02X", cfg->device_id);
    xTaskCreate(session_ctrl_task, name, 4096, s, 5, NULL);

    snprintf(name, sizeof(name), "sess_up_%02X", cfg->device_id);
    xTaskCreate(session_up_task, name, 3072, s, 4, NULL);

    snprintf(name, sizeof(name), "sess_down_%02X", cfg->device_id);
    xTaskCreate(session_down_task, name, 3072, s, 4, NULL);

    ESP_LOGI(TAG, "[dev=%02X] session tasks started", cfg->device_id);
}

EventGroupHandle_t device_session_event_group(uint8_t device_id)
{
    if (device_id >= SESSION_MAX_ID) return NULL;
    return s_sessions[device_id].eg;
}
