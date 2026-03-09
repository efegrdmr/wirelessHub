#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

// ── Event bits ────────────────────────────────────────────────────────────────
// Set when daemon has acknowledged this device (CMD_DEVICE_EVENT CONNECT sent,
// session is live and transferring data).
#define SESSION_ACTIVE_BIT  BIT0

// Set when the daemon connection was lost after a successful session.
// Cleared when a new session is established.
#define SESSION_LOST_BIT    BIT1

// ── Device ops ────────────────────────────────────────────────────────────────
// Implemented by the physical driver (eth_driver, usb_host, etc.).
// All calls happen from dedicated FreeRTOS tasks — they MAY block.
//
// recv() contract:
//   - Block until a frame arrives OR a driver-internal timeout (~1 s) elapses.
//   - Return number of bytes written to buf on success.
//   - Return ≤ 0 on timeout or error — the caller will re-check event bits.
//
// send() contract:
//   - Write the frame to the physical medium.
//   - Return true on success, false on unrecoverable error.
typedef struct {
    bool    (*init)(void);
    ssize_t (*recv)(uint8_t *buf, size_t max_len);
    bool    (*send)(const uint8_t *buf, size_t len);
} device_ops_t;

// ── Session configuration ─────────────────────────────────────────────────────
typedef struct {
    uint8_t             device_id;   // DEVICE_ID_ETHERNET, DEVICE_ID_USB0, etc.
    uint8_t             speed;       // USB_SPEED_* constant (Ethernet → USB_SPEED_HIGH)
    uint8_t             usb_class;   // USB class code (Ethernet → 0x02 CDC)
    uint8_t             subclass;
    uint8_t             protocol;
    const device_ops_t *ops;
} device_session_cfg_t;

// ── Public API ────────────────────────────────────────────────────────────────

/**
 * @brief Initialise and start a device session.
 *
 * Spawns three FreeRTOS tasks (ctrl / tx / rx) that run for the lifetime of
 * the firmware.  Safe to call immediately after hub_client_start(); the tasks
 * will block internally until DAEMON_FOUND_BIT is set.
 *
 * Can be called multiple times for different device_ids.
 */
void device_session_start(const device_session_cfg_t *cfg);

/**
 * @brief Return the EventGroup handle for a given device session.
 *
 * Valid bits:  SESSION_ACTIVE_BIT, SESSION_LOST_BIT.
 * Returns NULL if device_session_start() has not been called for that id.
 */
EventGroupHandle_t device_session_event_group(uint8_t device_id);
