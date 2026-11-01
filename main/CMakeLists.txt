idf_component_register(
    SRCS "main.c" "network.c" "web.c" "config_store.c" "management_settings.c" "time_service.c" "provisioning.c" "password_kdf.c" "console_line.c" "usb_console.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_driver_usb_serial_jtag esp_wifi esp_event esp_netif esp_timer esp_http_server esp_https_server nvs_flash lwip mbedtls
)
