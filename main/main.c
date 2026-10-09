#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"

#define RF_A_GPIO GPIO_NUM_4
#define RF_B_GPIO GPIO_NUM_5
#define RADXA_RESET_GPIO GPIO_NUM_6
#define RF_MUTE_GPIO GPIO_NUM_7

static const char *TAG = "rf_filter";
static uint8_t selected_port = 1;

/* Provisional pinout: verify against ESP32-S3 board before wiring.
 * Radxa reset output drives an EXTERNAL transistor/opto driver only.
 * Reset is active-low at this GPIO: high=inactive, low=asserted.
 * RF mute is active-high; hardware should default to muted on boot.
 */
static esp_err_t select_filter(uint8_t port)
{
    if (port < 1 || port > 4) return ESP_ERR_INVALID_ARG;
    gpio_set_level(RF_MUTE_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    uint8_t bits = port - 1;
    gpio_set_level(RF_A_GPIO, bits & 1);
    gpio_set_level(RF_B_GPIO, (bits >> 1) & 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(RF_MUTE_GPIO, 0);
    selected_port = port;
    ESP_LOGI(TAG, "Selected filter %u", (unsigned)selected_port);
    return ESP_OK;
}

void app_main(void)
{
    /* Preload safe output latches before enabling GPIO drivers. */
    gpio_set_level(RF_MUTE_GPIO, 1);
    gpio_set_level(RADXA_RESET_GPIO, 1);
    gpio_set_level(RF_A_GPIO, 0);
    gpio_set_level(RF_B_GPIO, 0);
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << RF_A_GPIO) | (1ULL << RF_B_GPIO) |
                        (1ULL << RADXA_RESET_GPIO) | (1ULL << RF_MUTE_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    ESP_ERROR_CHECK(select_filter(1));
    ESP_LOGW(TAG, "Baseline GPIO demo only: web control and Radxa reset API not implemented");
}
