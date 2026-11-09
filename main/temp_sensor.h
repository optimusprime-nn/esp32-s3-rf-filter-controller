#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
/* The internal sensor, an AHT20/AHT30 on each of the two I2C buses and up to eight
   LM75-family sensors on the first. */
#define RF_TEMP_MAX_LM75 8
#define RF_TEMP_MAX_SENSORS (3+RF_TEMP_MAX_LM75)
typedef enum { RF_TEMP_INTERNAL, RF_TEMP_LM75, RF_TEMP_AHT } rf_temp_source_t;
typedef struct {
    char id[16];
    rf_temp_source_t source;
    bool ok;
    bool has_humidity;          /* the sensor measures relative humidity (AHT20/AHT30) */
    uint8_t bus;                /* 1 or 2; 0 for the internal sensor */
    uint8_t address;
    int32_t millicelsius;
    int32_t millipercent_rh;    /* valid when has_humidity and ok */
} rf_temp_reading_t;
/* Starts the internal sensor and the I2C sensors that answer at boot, then reads them every 2 s. */
esp_err_t rf_temp_start(void);
/* Copies the latest readings; returns how many. */
size_t rf_temp_snapshot(rf_temp_reading_t *out, size_t max);
/* "internal", "lm75" or "aht". */
const char *rf_temp_source_name(rf_temp_source_t s);
