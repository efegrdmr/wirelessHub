#include "wifi_manager.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static const char *TAG = "WiFiMgr";

// Samsung + some Android versions require the AP IP to be in PUBLIC address space.
// 4.3.2.1 is the same address used by the reference CDFER captive portal impl.
#define AP_IP_STR     "4.3.2.1"
#define AP_IP_URL     "http://4.3.2.1"
#define AP_IP_API_URL "http://4.3.2.1/api"
#define WIFI_CHANNEL  6

// ── State ─────────────────────────────────────────────────────────────────────
static EventGroupHandle_t s_event_group  = NULL;
static httpd_handle_t     s_http_server  = NULL;
static int                s_retry_count  = 0;
static bool               s_netif_sta_created = false;
static bool               s_netif_ap_created  = false;

// DNS hijack server
static TaskHandle_t       s_dns_task     = NULL;
static int                s_dns_sock     = -1;


// ── NVS ───────────────────────────────────────────────────────────────────────
static bool nvs_load_credentials(char *ssid, size_t ssid_len,
                                  char *pass, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = (nvs_get_str(h, WIFI_NVS_KEY_SSID, ssid, &ssid_len) == ESP_OK) &&
              (nvs_get_str(h, WIFI_NVS_KEY_PASS,  pass, &pass_len) == ESP_OK);
    nvs_close(h);
    return ok;
}

static void nvs_save_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &h));
    ESP_ERROR_CHECK(nvs_set_str(h, WIFI_NVS_KEY_SSID, ssid));
    ESP_ERROR_CHECK(nvs_set_str(h, WIFI_NVS_KEY_PASS, pass));
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
}

// ── DNS hijack ────────────────────────────────────────────────────────────────
// Replies to every DNS query with AP_IP_STR so all domains point to the portal.
static void dns_server_task(void *arg)
{
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(53),
        .sin_addr.s_addr = INADDR_ANY,
    };
    s_dns_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_dns_sock < 0 || bind(s_dns_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS socket error");
        if (s_dns_sock >= 0) { close(s_dns_sock); s_dns_sock = -1; }
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "DNS running on :53");

    uint32_t ap_ip = inet_addr(AP_IP_STR);
    static uint8_t buf[512], resp[512];

    while (1) {
        struct sockaddr_in client;
        socklen_t clen = sizeof(client);
        int len = recvfrom(s_dns_sock, buf, sizeof(buf) - 1, 0,
                           (struct sockaddr *)&client, &clen);
        if (len < 12) break;

        // Extract queried hostname for logging
        char qname[128] = {0};
        int qi = 12, qo = 0;
        while (qi < len && buf[qi] && qo < (int)sizeof(qname) - 2) {
            uint8_t l = buf[qi++];
            if ((l & 0xC0) == 0xC0) break;
            for (int k = 0; k < l && qi < len; k++) qname[qo++] = buf[qi++];
            qname[qo++] = '.';
        }
        if (qo) qname[qo - 1] = '\0';
        ESP_LOGI(TAG, "DNS %s -> " AP_IP_STR, qname);

        // Find end of question section
        int ne = 12;
        while (ne < len && buf[ne]) {
            if ((buf[ne] & 0xC0) == 0xC0) { ne += 2; goto found; }
            ne += buf[ne] + 1;
        }
        ne++;  // consume null terminator
        found:;
        int qend = ne + 4;  // QTYPE(2) + QCLASS(2)
        if (qend > len) continue;
        int qlen = qend - 12;

        // Header
        resp[0]=buf[0]; resp[1]=buf[1];            // txid
        resp[2]=0x81;   resp[3]=0x80;              // QR=1, RCODE=0
        resp[4]=0;      resp[5]=1;                 // QDCOUNT=1
        resp[6]=0;      resp[7]=1;                 // ANCOUNT=1
        resp[8]=0;      resp[9]=0;                 // NSCOUNT=0
        resp[10]=0;     resp[11]=0;                // ARCOUNT=0
        memcpy(resp + 12, buf + 12, qlen);
        int pos = 12 + qlen;

        // Answer: A record, TTL=3, RDLENGTH=4
        resp[pos++]=0xC0; resp[pos++]=0x0C;        // name ptr → offset 12
        resp[pos++]=0x00; resp[pos++]=0x01;        // Type A
        resp[pos++]=0x00; resp[pos++]=0x01;        // Class IN
        resp[pos++]=0x00; resp[pos++]=0x00;
        resp[pos++]=0x00; resp[pos++]=0x03;        // TTL = 3 s
        resp[pos++]=0x00; resp[pos++]=0x04;        // RDLENGTH = 4
        memcpy(resp + pos, &ap_ip, 4); pos += 4;

        sendto(s_dns_sock, resp, pos, 0, (struct sockaddr *)&client, clen);
    }

    close(s_dns_sock);
    s_dns_sock = -1;
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

static void start_dns_server(void)
{
    if (s_dns_task) return;
    xTaskCreate(dns_server_task, "dns_hijack", 4096, NULL, 6, &s_dns_task);
}

static void stop_dns_server(void)
{
    if (s_dns_sock >= 0) {
        close(s_dns_sock); // unblocks recvfrom → task exits
        s_dns_sock = -1;
    }
    s_dns_task = NULL;
}

// ── URL decoder ───────────────────────────────────────────────────────────────
// Decodes a percent-encoded string in-place.
static void url_decode(char *dst, const char *src, size_t dst_len)
{
    size_t i = 0, j = 0;
    while (src[i] && j < dst_len - 1) {
        if (src[i] == '+') {
            dst[j++] = ' ';
            i++;
        } else if (src[i] == '%' && src[i+1] && src[i+2]) {
            char hex[3] = { src[i+1], src[i+2], '\0' };
            dst[j++] = (char)strtol(hex, NULL, 16);
            i += 3;
        } else {
            dst[j++] = src[i++];
        }
    }
    dst[j] = '\0';
}

// Extracts a field from an application/x-www-form-urlencoded body.
// "ssid=MyNet&pass=abc" with key="ssid" → writes "MyNet" into out.
static bool form_field(const char *body, const char *key,
                        char *out, size_t out_len)
{
    char search[64];
    snprintf(search, sizeof(search), "%s=", key);
    const char *p = strstr(body, search);
    if (!p) return false;
    p += strlen(search);
    const char *end = strchr(p, '&');
    size_t field_len = end ? (size_t)(end - p) : strlen(p);
    char raw[256] = {0};
    if (field_len >= sizeof(raw)) field_len = sizeof(raw) - 1;
    memcpy(raw, p, field_len);
    url_decode(out, raw, out_len);
    return true;
}

// ── HTTP handlers ─────────────────────────────────────────────────────────────
static const char *ROOT_HTML =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<title>WirelessHub Setup</title></head><body>"
    "<h2>WirelessHub WiFi Setup</h2>"
    "<form method='POST' action='/save'>"
    "Network (SSID):<br>"
    "<input name='ssid' type='text' maxlength='31' required><br><br>"
    "Password:<br>"
    "<input name='pass' type='password' maxlength='63'><br><br>"
    "<input type='submit' value='Connect'>"
    "</form></body></html>";

static esp_err_t send_portal_html(httpd_req_t *req)
{
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, ROOT_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// 404 catch-all: return the portal page for any unrecognised URL
static esp_err_t captive_redirect_handler(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    ESP_LOGW(TAG, "HTTP 404: %s -> portal", req->uri);
    return send_portal_html(req);
}

// Probe endpoints that simply show the portal (Android, iOS, Linux NM)
static esp_err_t portal_html_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "HTTP %s -> portal", req->uri);
    return send_portal_html(req);
}

// Redirect to portal root (used for /ncsi.txt, /redirect, /canonical.html)
static esp_err_t redirect_to_portal_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", AP_IP_URL);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// Windows 11: /connecttest.txt must redirect to http://logout.net specifically
static esp_err_t windows_connecttest_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://logout.net");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// Return 404 silently (/wpad.dat, /favicon.ico) to stop repeated probing
static esp_err_t not_found_handler(httpd_req_t *req)
{
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not Found");
    return ESP_OK;
}

// RFC 8908 Captive Portal API — DHCP Option 114 points here
static esp_err_t api_captive_handler(httpd_req_t *req)
{
    const char *json = "{\"captive\":true,\"user-portal-url\":\"" AP_IP_URL "/\"}";
    httpd_resp_set_type(req, "application/captive+json");
    httpd_resp_set_hdr(req, "Cache-Control", "private, no-store");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t post_save_handler(httpd_req_t *req)
{
    char body[512] = {0};
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }

    char ssid[32] = {0};
    char pass[64] = {0};

    if (!form_field(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SSID");
        return ESP_FAIL;
    }
    form_field(body, "pass", pass, sizeof(pass));

    ESP_LOGI(TAG, "Saving SSID=%s — restarting", ssid);
    nvs_save_credentials(ssid, pass);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, "<h3>Saved! Restarting...</h3>", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static void stop_http_server(void)
{
    if (s_http_server) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
}

static void start_http_server(void)
{
    if (s_http_server) return;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable  = true;
    cfg.max_uri_handlers  = 16;
    cfg.max_open_sockets  = 7;

    if (httpd_start(&s_http_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

#define REGISTER(u, m, h) do { \
        httpd_uri_t _r = {.uri=(u), .method=(m), .handler=(h)}; \
        httpd_register_uri_handler(s_http_server, &_r); \
    } while (0)

    REGISTER("/api",                     HTTP_GET,  api_captive_handler);        // RFC 8908
    REGISTER("/",                        HTTP_GET,  portal_html_handler);
    REGISTER("/save",                    HTTP_POST, post_save_handler);
    REGISTER("/generate_204",            HTTP_GET,  portal_html_handler);        // Android
    REGISTER("/hotspot-detect.html",     HTTP_GET,  portal_html_handler);        // iOS
    REGISTER("/check_network_status.txt",HTTP_GET,  portal_html_handler);        // Linux NM
    REGISTER("/connecttest.txt",         HTTP_GET,  windows_connecttest_handler); // Win11
    REGISTER("/ncsi.txt",                HTTP_GET,  redirect_to_portal_handler);
    REGISTER("/redirect",                HTTP_GET,  redirect_to_portal_handler);
    REGISTER("/canonical.html",          HTTP_GET,  redirect_to_portal_handler);
    REGISTER("/success.txt",             HTTP_GET,  redirect_to_portal_handler); // Firefox
    REGISTER("/wpad.dat",                HTTP_GET,  not_found_handler);          // Win10
    REGISTER("/favicon.ico",             HTTP_GET,  not_found_handler);

#undef REGISTER

    httpd_register_err_handler(s_http_server, HTTPD_404_NOT_FOUND,
                               captive_redirect_handler);

    ESP_LOGI(TAG, "HTTP server started — open " AP_IP_URL);
}

// ── WiFi modes ────────────────────────────────────────────────────────────────
static void start_ap_mode(void)
{
    ESP_LOGI(TAG, "Starting AP: %s", WIFI_AP_SSID);

    if (!s_netif_ap_created) {
        esp_netif_create_default_wifi_ap();
        s_netif_ap_created = true;
    }

    // Set AP IP to PUBLIC address space (4.3.2.1) BEFORE wifi start.
    // Samsung devices REQUIRE the gateway IP to be non-private.
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif) {
        esp_netif_dhcps_stop(ap_netif);
        esp_netif_ip_info_t ip_info = {};
        IP4_ADDR(&ip_info.ip,      4, 3, 2, 1);
        IP4_ADDR(&ip_info.gw,      4, 3, 2, 1);
        IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
        ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ip_info));
    }

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid            = WIFI_AP_SSID,
            .ssid_len        = strlen(WIFI_AP_SSID),
            .max_connection  = WIFI_AP_MAX_CONN,
            .authmode        = WIFI_AUTH_OPEN,
            .channel         = WIFI_CHANNEL,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    vTaskDelay(pdMS_TO_TICKS(100));

    if (ap_netif) {
        // DNS server advertisement → 4.3.2.1
        esp_netif_dns_info_t dns = {};
        IP4_ADDR(&dns.ip.u_addr.ip4, 4, 3, 2, 1);
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        ESP_ERROR_CHECK(esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns));

        // RFC 8910 DHCP Option 114 → Captive Portal API (RFC 8908)
        esp_netif_dhcps_option(ap_netif,
                               ESP_NETIF_OP_SET,
                               ESP_NETIF_CAPTIVEPORTAL_URI,
                               (void *)AP_IP_API_URL,
                               strlen(AP_IP_API_URL));

        esp_netif_dhcps_start(ap_netif);
        ESP_LOGI(TAG, "DHCP: DNS=" AP_IP_STR ", CaptivePortal=" AP_IP_API_URL);
    }

    start_http_server();
    start_dns_server();

    xEventGroupClearBits(s_event_group, WIFI_READY_BIT);
    xEventGroupSetBits(s_event_group, WIFI_LOST_BIT);
}

static void start_sta_mode(const char *ssid, const char *pass)
{
    ESP_LOGI(TAG, "Connecting to: %s", ssid);

    if (!s_netif_sta_created) {
        esp_netif_create_default_wifi_sta();
        s_netif_sta_created = true;
    }

    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid,     ssid, sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password) - 1);
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN; // allow open networks too

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
}

// ── Event handler ─────────────────────────────────────────────────────────────
static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {

        case WIFI_EVENT_STA_START:
            // triggered by esp_wifi_start() in STA mode — immediately connect
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            xEventGroupClearBits(s_event_group, WIFI_READY_BIT);
            xEventGroupSetBits(s_event_group,   WIFI_LOST_BIT);

            s_retry_count++;
            if (s_retry_count <= WIFI_MAX_RETRY) {
                ESP_LOGW(TAG, "Disconnected — retry %d/%d",
                         s_retry_count, WIFI_MAX_RETRY);
                esp_wifi_connect();
            } else {
                ESP_LOGE(TAG, "Max retries reached — falling back to AP mode");
                s_retry_count = 0;
                start_ap_mode();
            }
            break;
        }

        case WIFI_EVENT_AP_STACONNECTED:
            ESP_LOGI(TAG, "A device connected to the setup AP — heap free=%lu",
                     esp_get_free_heap_size());
            break;

        default:
            break;
        }

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&ev->ip_info.ip));

        s_retry_count = 0;
        stop_dns_server();
        stop_http_server();

        xEventGroupClearBits(s_event_group, WIFI_LOST_BIT);
        xEventGroupSetBits(s_event_group,   WIFI_READY_BIT);
    }
}

// ── wifi_manager_task ─────────────────────────────────────────────────────────
static void wifi_manager_task(void *arg)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    init_cfg.ampdu_rx_enable = 0;  // Android AMPDU RX bug workaround
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID,   wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT,   IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    char ssid[32] = {0}, pass[64] = {0};
    if (nvs_load_credentials(ssid, sizeof(ssid), pass, sizeof(pass)))
        start_sta_mode(ssid, pass);
    else
        start_ap_mode();

    vTaskDelete(NULL);
}

// ── Public API ────────────────────────────────────────────────────────────────
void wifi_manager_start(void)
{
    s_event_group = xEventGroupCreate();
    xTaskCreate(wifi_manager_task, "wifi_mgr", 4096, NULL, 5, NULL);
}

EventGroupHandle_t wifi_manager_event_group(void)
{
    return s_event_group;
}
