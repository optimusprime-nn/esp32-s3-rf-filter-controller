#pragma once
/* Fixed-size ring of weather samples, addressed by a sequence number that keeps counting
   so a reader working in pieces can tell when older samples were overwritten. Not locked. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "weather_decode.h"

#define RF_WEATHER_HISTORY_INTERVAL_S 60

typedef struct {
    uint32_t t_s;          /* seconds since boot */
    int16_t temp_dc;
    uint16_t wind_raw;
    uint16_t wind_dir_deg;
    uint16_t rain_raw;
    uint32_t light_lux;
    uint8_t humidity;
    uint8_t gust_raw;
    int8_t uv_index;
    int8_t rssi_dbm;
} rf_weather_sample_t;

typedef struct {
    rf_weather_sample_t *buf;
    size_t cap;
    uint32_t next_seq;     /* sequence number the next sample gets */
    bool any;
    uint32_t last_t_s;
} rf_weather_history_t;

void rf_weather_history_init(rf_weather_history_t *h, rf_weather_sample_t *buf, size_t cap);
/* Records a reading unless the last sample is under RF_WEATHER_HISTORY_INTERVAL_S old. Returns true if recorded. */
bool rf_weather_history_add(rf_weather_history_t *h, const rf_vevor_reading_t *r, int rssi_dbm, uint32_t t_s);
/* Oldest sequence number still held. Samples held are [oldest, next_seq). */
uint32_t rf_weather_history_oldest(const rf_weather_history_t *h);
/* Copies up to max samples starting at seq, clamped to what is held; returns the count and
   sets *next to the sequence number after the last one copied. */
size_t rf_weather_history_read(const rf_weather_history_t *h, uint32_t seq, rf_weather_sample_t *out, size_t max, uint32_t *next);
