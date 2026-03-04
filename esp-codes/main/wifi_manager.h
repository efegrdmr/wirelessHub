#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

// ── Event bits ────────────────────────────────────────────────────────────────
// Set when STA is connected and has an IP address.
// Clear when STA disconnects or falls back to AP mode.
#define WIFI_READY_BIT  BIT0

// Set when STA disconnects unexpectedly. Clear when reconnected.
#define WIFI_LOST_BIT   BIT1

// ── Config ────────────────────────────────────────────────────────────────────
#define WIFI_MAX_RETRY      5                    // STA fail → AP fallback after this
#define WIFI_AP_SSID        "WirelessHub-Setup"  // captive portal network name
#define WIFI_AP_MAX_CONN    4                    // concurrent AP clients
#define WIFI_NVS_NAMESPACE  "wifi_creds"
#define WIFI_NVS_KEY_SSID   "ssid"
#define WIFI_NVS_KEY_PASS   "pass"

// ── Public API ────────────────────────────────────────────────────────────────

/**
 * Initialise the WiFi manager.
 * - Creates the shared EventGroup.
 * - Spawns wifi_manager_task which checks NVS and starts STA or AP mode.
 * - Must be called after nvs_flash_init().
 */
void wifi_manager_start(void);

/**
 * Returns the EventGroup handle so other tasks can wait on WIFI_READY_BIT
 * or WIFI_LOST_BIT without depending on any global variable.
 */
EventGroupHandle_t wifi_manager_event_group(void);
