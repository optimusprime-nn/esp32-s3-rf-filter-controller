#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#define RF_TEMP_MAX_SENSORS 9
typedef struct {
    char id[16];
    bool internal, ok;
    uint8_t address;
    int32_t millicelsius;
} rf_temp_reading_t;
/* Starts the internal sensor and the I2C sensors that answer at boot, then reads them every 2 s. */
esp_err_t rf_temp_start(void);
/* Copies the latest readings; returns how many. */
size_t rf_temp_snapshot(rf_temp_reading_t *out, size_t max);
