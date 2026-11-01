#pragma once
#include <stdint.h>
#include "driver/gpio.h"
#include "esp_err.h"
#define RF_A_GPIO GPIO_NUM_4
#define RF_B_GPIO GPIO_NUM_5
#define RF_MUTE_GPIO GPIO_NUM_7
/* Applies the port read at boot. Call once, before anything else can select a filter. */
esp_err_t rf_filter_start(uint8_t port);
/* Switches to port and saves it for the next boot. Safe from any task. */
esp_err_t rf_filter_select(uint8_t port);
uint8_t rf_filter_current(void);
