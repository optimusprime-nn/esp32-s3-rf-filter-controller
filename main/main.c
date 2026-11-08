#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_err.h"
#include "management_settings.h"
#include "time_service.h"
#include "filter.h"
#include "ownership.h"
#include "fan.h"
#include "temp_sensor.h"
#include "weather.h"
#include "sdr_power.h"
extern esp_err_t config_store_init(void);
extern esp_err_t config_store_load_port(uint8_t *port);
extern esp_err_t network_start(const rf_management_settings_t *settings);
extern esp_err_t web_start(void);
extern esp_err_t rf_usb_console_start(void);
static const char *TAG="rf_filter";
void app_main(void) {
    /* First, so the SDR's reboot line and power relay are idle as early as possible. */
    esp_err_t e=rf_sdr_start();
    if(e!=ESP_OK) ESP_LOGE(TAG,"SDR reset lines unavailable: %s",esp_err_to_name(e));
    gpio_set_level(RF_MUTE_GPIO,1);
    gpio_set_level(RF_A_GPIO,0);
    gpio_set_level(RF_B_GPIO,0);
    gpio_config_t cfg={
        .pin_bit_mask=(1ULL<<RF_A_GPIO)|(1ULL<<RF_B_GPIO)|(1ULL<<RF_MUTE_GPIO),
        .mode=GPIO_MODE_OUTPUT,.pull_up_en=GPIO_PULLUP_DISABLE,
        .pull_down_en=GPIO_PULLDOWN_DISABLE,.intr_type=GPIO_INTR_DISABLE};
    ESP_ERROR_CHECK(gpio_config(&cfg));
    /* A panic here would reboot-loop and release the RF mute output on every reset. */
    e=config_store_init();
    if(e!=ESP_OK) { ESP_LOGE(TAG,"NVS unavailable, staying offline: %s",esp_err_to_name(e)); return; }
    /* Prints the enrollment code on an unclaimed board, for the setup tool to read. */
    e=rf_ownership_init();
    if(e!=ESP_OK) ESP_LOGE(TAG,"Management ownership state unreadable: %s",esp_err_to_name(e));
    uint8_t port=1;
    if(config_store_load_port(&port)!=ESP_OK) port=1;
    ESP_ERROR_CHECK(rf_filter_start(port));
    /* Not fatal: with the recommended driver an unconfigured PWM pin leaves the fan at full speed. */
    e=rf_fan_start();
    if(e!=ESP_OK) ESP_LOGE(TAG,"Fan control unavailable: %s",esp_err_to_name(e));
    e=rf_temp_start();
    if(e!=ESP_OK) ESP_LOGW(TAG,"Temperature sensors unavailable: %s",esp_err_to_name(e));
    e=rf_weather_start();
    if(e!=ESP_OK) ESP_LOGW(TAG,"Weather receiver unavailable: %s",esp_err_to_name(e));
    /* Before anything below can return early: an unprovisioned device must still be reachable over USB. */
    e=rf_usb_console_start();
    if(e!=ESP_OK) ESP_LOGW(TAG,"USB provisioning console unavailable: %s",esp_err_to_name(e));
    rf_management_settings_t settings;
    e=rf_settings_load(&settings);
    if(e!=ESP_OK) { ESP_LOGE(TAG,"Invalid stored network settings; network disabled"); return; }
    e=network_start(&settings);
    if(e!=ESP_OK) { ESP_LOGW(TAG,"Network unavailable: %s",esp_err_to_name(e)); return; }
    ESP_ERROR_CHECK(rf_time_service_start(settings.ntp_servers[0],settings.ntp_servers[1]));
    /* Enrollment only until claimed, then mTLS; see web.c. */
    e=web_start();
    if(e!=ESP_OK) ESP_LOGW(TAG,"HTTPS management unavailable: %s",esp_err_to_name(e));
    /* Headroom check: startup overflowed the default 3.5 KB main task stack. */
    ESP_LOGI(TAG,"Main task stack unused: %u bytes",(unsigned)uxTaskGetStackHighWaterMark(NULL));
}
