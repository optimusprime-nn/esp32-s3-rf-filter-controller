idf_component_register(
    SRCS "main.c" "network.c" "web.c" "config_store.c" "management_settings.c" "time_service.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_wifi esp_event esp_netif esp_http_server esp_https_server nvs_flash lwip
)
