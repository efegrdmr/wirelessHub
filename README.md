# WirelessHub

WirelessHub enables wireless keyboard and mouse connectivity between your computer and a remote device. The project consists of two main parts:

- **daemon/** (C++):
   - Runs on Linux, manages network communication, protocol handling, and USB/IP emulation.
   - Key modules:
      - `handlers/`: Contains `PassthroughHandler` for bridging USB/IP data (passthrough not yet active).
      - `network/`: Implements `HubServer` (UDP/TCP server), `TcpSocket`/`UdpSocket` abstractions, and `TapDevice` for virtual network interfaces.
      - `protocol/`: Defines the shared protocol (`Protocol.h`) for communication.
      - `usbip/`: `VhciDriver` manages Linux vhci-hcd for USB/IP emulation.
      - `main.cpp`: Initializes and runs the main event loop.

- **esp-codes/** (ESP32, C):
   - ESP-IDF firmware for the remote device.
   - Key modules:
      - `usb_driver.[ch]`: USB host stack, reads HID (keyboard/mouse) frames.
      - `hub_client.[ch]`: Handles discovery and connection to the daemon.
      - `usb_passthrough.[ch]`: Intended for passthrough (not yet active).
      - `device_session.[ch]`: Manages device sessions and protocol operations.
      - `log_forwarder.[ch]`: Forwards logs to the daemon.
      - `wifi_manager.[ch]`: Manages WiFi.

## Protocol & Data Flow

## FreeRTOS Structure (ESP32 Firmware)

The ESP32 firmware (esp-codes/) is built on ESP-IDF and uses FreeRTOS for multitasking:

- **Task-based architecture:**
   - Separate FreeRTOS tasks (threads) are created for:
      - USB hardware event handling (`usb_lib_task`)
      - USB client event processing (`usb_client_task`)
      - Hardware/connection watchdog monitoring (`usb_watchdog_task`)
- **Inter-task communication:**
   - USB data is passed between tasks using FreeRTOS queues (e.g., `s_rx_queue`).
   - Semaphores (`s_out_done`, `s_ctrl_done`) are used to synchronize and signal transfer completions.
- **Non-blocking, responsive design:**
   - Each task runs in an infinite loop, waiting for events or messages.
   - Periodic operations (like watchdog checks) use FreeRTOS timers (`vTaskDelay`).

This structure allows the ESP32 to handle USB, network, and device management operations concurrently and reliably.

- ESP32 first starts as an access point (AP) for WiFi configuration. After entering WiFi credentials, it connects to the target WiFi network and then broadcasts DISCOVER via UDP.
- Daemon replies, connection established.
- ESP32 reads HID reports, sends as protocol packets to daemon.
- Daemon is designed to inject these into Linux USB/IP stack (passthrough not yet active).
- Only keyboard and mouse data are supported.

## Limitations

- **Passthrough mode is not working** (the code exists but is not implemented or functional yet).
- Only keyboard and mouse (HID) are supported; no other USB classes.
- Daemon is Linux-only.
- Single device/session focus.
