set(srcs "app_main.c"
         "wifi_manager.c")

idf_component_register(SRCS ${srcs}
    INCLUDE_DIRS "."
    PRIV_REQUIRES nvs_flash esp_wifi esp_netif esp_http_server esp_event)
