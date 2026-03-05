#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

// ── Event bits ────────────────────────────────────────────────────────────────
// Set when a daemon has successfully replied to a DISCOVER broadcast.
// Clear when the daemon becomes unreachable (future: heartbeat timeout).
#define DAEMON_FOUND_BIT  BIT0

// Set when a previously-found daemon becomes unreachable.
// Clear when a new DISCOVER cycle succeeds.
#define DAEMON_LOST_BIT   BIT1

// ── Config ────────────────────────────────────────────────────────────────────
#define DISCOVER_TIMEOUT_SEC  3   // seconds to wait for a DISCOVER_REPLY before retrying
#define DISCOVER_RETRY_SEC   5   // seconds to wait between full discovery cycles

/**
 * @brief Start the hub daemon discovery client.
 *
 * Spawns a FreeRTOS task that waits for WIFI_READY_BIT, then sends UDP
 * broadcast DISCOVER packets until a DISCOVER_REPLY is received from the
 * daemon.  Safe to call immediately after wifi_manager_start().
 */
void hub_client_start(void);

/**
 * @brief Returns the hub client EventGroup handle.
 *
 * Bits defined in hub_client.h:
 *   DAEMON_FOUND_BIT — daemon discovered and reachable
 *   DAEMON_LOST_BIT  — daemon was lost (future use)
 *
 * Example:
 *   xEventGroupWaitBits(hub_client_event_group(), DAEMON_FOUND_BIT,
 *                       pdFALSE, pdTRUE, portMAX_DELAY);
 */
EventGroupHandle_t hub_client_event_group(void);

/**
 * @brief Returns true once a daemon has replied to a DISCOVER broadcast.
 *        Prefer waiting on DAEMON_FOUND_BIT via hub_client_event_group().
 */
bool hub_client_daemon_found(void);

/**
 * @brief Daemon IPv4 address in network byte order.
 *        Valid only after hub_client_daemon_found() returns true.
 */
uint32_t hub_client_daemon_ip(void);

/**
 * @brief Daemon UDP port in host byte order (typically DAEMON_PORT = 7788).
 *        Valid only after hub_client_daemon_found() returns true.
 */
uint16_t hub_client_daemon_port(void);
