#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

// ── Event bits ────────────────────────────────────────────────────────────────
// Set when STA is connected and has an IP address.
// Clear when STA disconnects or falls back to AP mode.
#define WIFI_READY_BIT      BIT0

// Set when STA disconnects unexpectedly. Clear when reconnected.
#define WIFI_LOST_BIT       BIT1

// Set when captive-portal AP mode is active. Clear when STA gets an IP.
#define WIFI_AP_MODE_BIT    BIT2

// Set when a STA connect attempt is in progress. Clear on success or AP fallback.
#define WIFI_CONNECTING_BIT BIT3

// Set when STA fails due to wrong credentials (auth or handshake failure).
// Clear when a new STA connect attempt starts or WIFI_READY_BIT is set.
#define WIFI_AUTH_FAIL_BIT  BIT4

// ── Config ────────────────────────────────────────────────────────────────────
#define WIFI_MAX_RETRY      5                    // STA fail → AP fallback after this
#define WIFI_AP_SSID        "WirelessHub-Setup"  // captive portal network name
#define WIFI_AP_MAX_CONN    1                    // concurrent AP clients
#define WIFI_NVS_NAMESPACE  "wifi_creds"
#define WIFI_NVS_KEY_SSID   "ssid"
#define WIFI_NVS_KEY_PASS   "pass"

// Samsung + some Android versions require the AP IP to be in PUBLIC address space.
// 4.3.2.1 is the same address used by the reference CDFER captive portal impl.
#define WIFI_AP_IP_STR      "4.3.2.1"
#define WIFI_AP_IP_URL      "http://4.3.2.1"
#define WIFI_AP_IP_API_URL  "http://4.3.2.1/api"
#define WIFI_CHANNEL        6                    // AP channel

// ── Public API ────────────────────────────────────────────────────────────────

/**
 * Initialise the WiFi manager.
 * - Creates the shared EventGroup.
 * - Spawns wifi_manager_task which checks NVS and starts STA or AP mode.
 * - Must be called after nvs_flash_init().
 */
void wifi_manager_start(void);

/**
 * Returns the EventGroup handle so other tasks can wait on any WIFI_*_BIT
 * without depending on any global variable.
 *
 * Bits defined in this header:
 *   WIFI_READY_BIT      — STA connected and has IP
 *   WIFI_LOST_BIT       — STA disconnected
 *   WIFI_AP_MODE_BIT    — captive-portal AP is active
 *   WIFI_CONNECTING_BIT — STA connect attempt in progress
 *   WIFI_AUTH_FAIL_BIT  — last disconnect was an authentication failure
 */
EventGroupHandle_t wifi_manager_event_group(void);
