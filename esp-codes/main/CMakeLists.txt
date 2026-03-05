set(srcs "app_main.c"
         "wifi_manager.c"
         "hub_client.c")

# Shared Protocol.h lives in the daemon tree; expose it to C firmware via
# an additional include path so both sides compile from the same header.
set(PROTOCOL_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../daemon/src/protocol")

idf_component_register(SRCS ${srcs}
    INCLUDE_DIRS "." "${PROTOCOL_INCLUDE_DIR}"
    PRIV_REQUIRES nvs_flash esp_wifi esp_netif esp_http_server esp_event lwip)
