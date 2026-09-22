idf_component_register(
    SRCS "net_bridge.c"
    INCLUDE_DIRS "include" "."
    REQUIRES esp_wifi esp_netif esp_http_server esp_event nvs_flash freertos esp_timer sd_logger
)

