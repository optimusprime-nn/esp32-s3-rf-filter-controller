#include "filter.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
extern esp_err_t config_store_save_port(uint8_t port);
static const char *TAG="rf_filter";
static SemaphoreHandle_t lock;
static volatile uint8_t current;
static esp_err_t apply(uint8_t port) {
    esp_err_t e=gpio_set_level(RF_MUTE_GPIO,1);
    vTaskDelay(pdMS_TO_TICKS(10));
    uint8_t bits=port-1;
    if(e==ESP_OK) e=gpio_set_level(RF_A_GPIO,bits&1);
    if(e==ESP_OK) e=gpio_set_level(RF_B_GPIO,(bits>>1)&1);
    vTaskDelay(pdMS_TO_TICKS(10));
    /* RF mute release intentionally withheld pending fail-safe external hardware validation. */
    if(e==ESP_OK) { current=port; ESP_LOGI(TAG,"Selected filter %u",(unsigned)port); }
    return e;
}
esp_err_t rf_filter_start(uint8_t port) {
    if(port<1 || port>4) return ESP_ERR_INVALID_ARG;
    if(!lock) lock=xSemaphoreCreateMutex();
    if(!lock) return ESP_ERR_NO_MEM;
    return apply(port);
}
esp_err_t rf_filter_select(uint8_t port) {
    if(port<1 || port>4) return ESP_ERR_INVALID_ARG;
    if(!lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(lock,portMAX_DELAY);
    esp_err_t e=apply(port);
    if(e==ESP_OK) e=config_store_save_port(port);
    xSemaphoreGive(lock);
    return e;
}
uint8_t rf_filter_current(void) {
    return current;
}
