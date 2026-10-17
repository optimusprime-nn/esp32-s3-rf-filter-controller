#pragma once
#include <stdint.h>
#include "esp_err.h"
#define RF_FAN_COUNT 2
/* Both fans start at 100%; only call set_duty after verifying the external PWM interface. */
esp_err_t rf_fans_init(void);
esp_err_t rf_fan_set_duty(unsigned fan_index, uint8_t percent);
uint8_t rf_fan_get_duty(unsigned fan_index);
