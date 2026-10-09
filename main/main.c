#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_err.h"
#include "management_settings.h"
#include "time_service.h"
#define RF_A_GPIO GPIO_NUM_4
#define RF_B_GPIO GPIO_NUM_5
#define RADXA_RESET_GPIO GPIO_NUM_6
#define RF_MUTE_GPIO GPIO_NUM_7
extern esp_err_t config_store_init(void);
extern esp_err_t config_store_load_port(uint8_t *port);
extern esp_err_t network_start(const rf_management_settings_t *settings);
extern esp_err_t web_start(void);
static const char *TAG="rf_filter";
static esp_err_t select_filter(uint8_t port) {
    if(port<1 || port>4) return ESP_ERR_INVALID_ARG;
    ESP_ERROR_CHECK(gpio_set_level(RF_MUTE_GPIO,1));
    vTaskDelay(pdMS_TO_TICKS(10));
    uint8_t bits=port-1;
    ESP_ERROR_CHECK(gpio_set_level(RF_A_GPIO,bits&1));
    ESP_ERROR_CHECK(gpio_set_level(RF_B_GPIO,(bits>>1)&1));
    vTaskDelay(pdMS_TO_TICKS(10));
    /* RF mute release intentionally withheld pending fail-safe external hardware validation. */
    ESP_LOGW(TAG,"Selected filter %u (RF source inhibit remains asserted)",(unsigned)port);
    return ESP_OK;
}
void app_main(void) {
    gpio_set_level(RF_MUTE_GPIO,1);
    gpio_set_level(RADXA_RESET_GPIO,1);
    gpio_set_level(RF_A_GPIO,0);
    gpio_set_level(RF_B_GPIO,0);
    gpio_config_t cfg={
        .pin_bit_mask=(1ULL<<RF_A_GPIO)|(1ULL<<RF_B_GPIO)|(1ULL<<RADXA_RESET_GPIO)|(1ULL<<RF_MUTE_GPIO),
        .mode=GPIO_MODE_OUTPUT,.pull_up_en=GPIO_PULLUP_DISABLE,
        .pull_down_en=GPIO_PULLDOWN_DISABLE,.intr_type=GPIO_INTR_DISABLE};
    ESP_ERROR_CHECK(gpio_config(&cfg));
    ESP_ERROR_CHECK(config_store_init());
    uint8_t port=1;
    if(config_store_load_port(&port)!=ESP_OK) port=1;
    ESP_ERROR_CHECK(select_filter(port));
    rf_management_settings_t settings;
    esp_err_t e=rf_settings_load(&settings);
    if(e!=ESP_OK) { ESP_LOGE(TAG,"Invalid stored network settings; network disabled"); return; }
    e=network_start(&settings);
    if(e!=ESP_OK) { ESP_LOGW(TAG,"Network unavailable: %s",esp_err_to_name(e)); return; }
    ESP_ERROR_CHECK(rf_time_service_start(settings.ntp_server));
    /* Read-only diagnostics only. No unauthenticated state-changing endpoints. */
    e=web_start();
    if(e!=ESP_OK) ESP_LOGW(TAG,"HTTPS management unavailable (TLS provisioning required): %s",esp_err_to_name(e));
}
