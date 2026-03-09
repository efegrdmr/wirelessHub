#pragma once

/**
 * @file eth_passthrough.h
 *
 * Bootstrap for the Ethernet → daemon passthrough feature.
 *
 * eth_passthrough_start() initialises the Ethernet driver and registers
 * DEVICE_ID_ETHERNET with the device_session module.  The three session tasks
 * (ctrl / tx / rx) are started automatically.
 *
 * Call once from app_main(), after hub_client_start().
 */
void eth_passthrough_start(void);
