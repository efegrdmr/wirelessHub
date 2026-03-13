set(srcs "app_main.c"
         "wifi_manager.c"
         "hub_client.c"
         "device_session.c"
         "eth_driver.c"
         "eth_passthrough.c"
         "usb_driver.c"
         "usb_passthrough.c"
         "log_forwarder.c"
         "crash_reporter.c")

# Shared Protocol.h lives in the daemon tree; expose it to C firmware via
# an additional include path so both sides compile from the same header.
set(PROTOCOL_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../daemon/src/protocol")

# Use ETH_MOCK_DRIVER=1 for development / CI without real W5500 hardware.
# Remove this line (or set to 0) to compile the real W5500 SPI driver.
set(ETH_MOCK_DRIVER 0)

idf_component_register(SRCS ${srcs}
    INCLUDE_DIRS "." "${PROTOCOL_INCLUDE_DIR}"
    PRIV_REQUIRES nvs_flash esp_wifi esp_netif esp_http_server esp_event lwip
                  esp_eth driver usb)

if(ETH_MOCK_DRIVER)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ETH_MOCK_DRIVER)
endif()
