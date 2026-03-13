#pragma once

/**
 * @brief Redirect all ESP_LOG* output to the WirelessHub daemon via UDP.
 *
 * After this call, every ESP_LOGE/W/I/D/V line is:
 *   1. Printed to serial as normal.
 *   2. Serialised into a CMD_LOG datagram and sent unicast to the daemon.
 *
 * Pre-conditions:
 *   - hub_client_start() must have been called before log_forwarder_init().
 *   - Actual forwarding begins only once hub_client_daemon_found() is true;
 *     log lines emitted before the daemon is discovered are silently dropped.
 *
 * Safe to call from app_main() after hub_client_start().
 */
void log_forwarder_init(void);
