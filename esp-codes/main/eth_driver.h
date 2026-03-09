#pragma once

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include "device_session.h"

/**
 * @file eth_driver.h
 *
 * Thin abstraction over the W5500 SPI Ethernet chip (or a mock for CI/testing).
 * Exposes a device_ops_t-compatible interface consumable by device_session.
 *
 * Build-time switch:
 *   ETH_MOCK_DRIVER  – compile in the loopback mock (no hardware needed)
 *   (default)        – compile the real W5500 / esp_eth driver
 *
 * SPI pin defaults (real driver only) — override in sdkconfig or compile flags:
 *   ETH_SPI_HOST     HSPI_HOST  (SPI2)
 *   ETH_PIN_MOSI     13
 *   ETH_PIN_MISO     12
 *   ETH_PIN_SCLK     14
 *   ETH_PIN_CS       8
 *   ETH_PIN_INT      -1  (not connected)
 *   ETH_PIN_RST      -1  (not connected)
 *   ETH_SPI_CLOCK_MHZ 25
 */

#ifndef ETH_SPI_HOST
#define ETH_SPI_HOST       SPI2_HOST
#endif
#ifndef ETH_PIN_MOSI
#define ETH_PIN_MOSI       13
#endif
#ifndef ETH_PIN_MISO
#define ETH_PIN_MISO       12
#endif
#ifndef ETH_PIN_SCLK
#define ETH_PIN_SCLK       14
#endif
#ifndef ETH_PIN_CS
#define ETH_PIN_CS          8
#endif
#ifndef ETH_PIN_INT
#define ETH_PIN_INT        -1   /* not connected */
#endif
#ifndef ETH_PIN_RST
#define ETH_PIN_RST        -1   /* not connected */
#endif
#ifndef ETH_SPI_CLOCK_MHZ
#define ETH_SPI_CLOCK_MHZ  25
#endif

/**
 * device_ops_t for Ethernet.
 *
 * .init  – Configures SPI + W5500 (or mock) and starts the driver.
 * .recv  – Blocks until an Ethernet frame arrives (or ~1 s timeout).
 *          Returns frame length on success, -1 on timeout/error.
 * .send  – Transmits one Ethernet frame.  Returns true on success.
 */
extern const device_ops_t ETH_DRIVER_OPS;
