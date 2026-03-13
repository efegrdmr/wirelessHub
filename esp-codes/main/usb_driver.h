#pragma once

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include "device_session.h"

/**
 * @file usb_driver.h
 *
 * USB Host driver for passthrough — reads HID/CDC interrupt-IN frames from a
 * connected USB device and forwards them to the daemon via device_session.
 *
 * Uses ESP-IDF usb_host library (GPIO 19 = D-, GPIO 20 = D+).
 *
 * Exposes a device_ops_t interface consumable by device_session_start().
 *
 * .init  — Installs USB Host stack and starts background tasks. Returns
 *           immediately; device connection is handled asynchronously.
 * .recv  — Returns next IN-transfer frame from the RX queue (≤1 s timeout).
 *           Returns frame length on success, -1 on timeout / no device.
 * .send  — Sends a frame to the device via interrupt/bulk OUT endpoint.
 *           Returns true on success, false if no OUT pipe or on error.
 */

/* Depth of the internal RX frame queue. */
#ifndef USB_RX_QUEUE_DEPTH
#define USB_RX_QUEUE_DEPTH   8
#endif

/* Maximum raw HID/CDC frame size we expect from IN transfers. */
#ifndef USB_FRAME_MAX
#define USB_FRAME_MAX        512
#endif

extern const device_ops_t USB_DRIVER_OPS;

/**
 * @brief Send a USB OUT transfer and block until it completes (max 1 s).
 *
 * Unlike USB_DRIVER_OPS.send() which silently drops if no OUT pipe exists,
 * this returns the ESP-IDF USB transfer status:
 *   0                               — success
 *   USB_TRANSFER_STATUS_NO_DEVICE   — device gone
 *   USB_TRANSFER_STATUS_ERROR       — USB error
 *   ESP_ERR_TIMEOUT                 — transfer did not complete within 1 s
 *
 * Must NOT be called from an ISR / USB library task.
 */
int usb_drv_send_blocking(const uint8_t *buf, size_t len);

/**
 * @brief Perform a synchronous EP0 control transfer.
 *
 * @param setup       8-byte USB setup packet (host byte order, copied as-is)
 * @param out_data    Data for OUT stage; NULL for IN or zero-data transfers
 * @param out_len     Length of out_data (0 for IN or zero-data)
 * @param in_buf      Buffer for IN data; NULL for OUT or zero-data transfers
 * @param max_in      Size of in_buf
 * @return            Bytes received (IN) / 0 (OUT success) / negative on error
 *
 * Must NOT be called from an ISR / USB library task.
 */
int usb_drv_control(const uint8_t setup[8], const uint8_t *out_data, int out_len,
                    uint8_t *in_buf, int max_in);

/**
 * @brief Returns true when a USB device is physically connected and enumerated.
 * Safe to call from any task.
 */
bool usb_drv_is_device_ready(void);
