#pragma once
#include <stdint.h>
#include "esp_err.h"
/* Starts PWM at the saved speed and, if configured, the tachometer. */
esp_err_t rf_fan_start(void);
/* Sets speed in percent and saves it for the next boot. Safe from any task. */
esp_err_t rf_fan_set(uint8_t percent);
uint8_t rf_fan_percent(void);
/* Last measured speed, or -1 when no tachometer is configured or it failed to start. */
int32_t rf_fan_rpm_now(void);
