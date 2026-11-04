idf_component_register(
    SRCS "main.c" "network.c" "web.c" "config_store.c" "management_settings.c" "time_service.c" "provisioning.c" "console_line.c" "usb_console.c" "filter.c" "ownership.c" "bootstrap_tls.c" "der_util.c" "fan.c" "fan_math.c" "temp_sensor.c" "temp_math.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "www/index.html"
    REQUIRES esp_driver_gpio esp_driver_ledc esp_driver_pcnt esp_driver_i2c esp_driver_tsens esp_driver_usb_serial_jtag esp_wifi esp_event esp_netif esp_timer esp_http_server esp_https_server nvs_flash lwip mbedtls esp_app_format
)
