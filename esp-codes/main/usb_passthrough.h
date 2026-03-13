#pragma once

/**
 * @file usb_passthrough.h
 *
 * Bootstrap for the USB → daemon passthrough feature.
 *
 * usb_passthrough_start() initialises the USB Host driver and registers
 * DEVICE_ID_USB0 with the device_session module.  The three session tasks
 * (ctrl / up / down) are started automatically.
 *
 * Call once from app_main(), after hub_client_start().
 */
void usb_passthrough_start(void);
