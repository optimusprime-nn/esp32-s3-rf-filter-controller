#pragma once
/* Decoder for the VEVOR 7-in-1 weather station (YT60231 868 MHz, YT60233/YT60234 915 MHz,
   made by Fujian Youtong). Protocol as documented by rtl_433's vevor_7in1 decoder. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Bytes after the AA..CA CA 54 preamble and sync word. */
#define RF_VEVOR_PACKET_LEN 21

typedef struct {
    uint16_t id;
    uint8_t channel;
    bool battery_low;      /* bit 7 of the flags byte; rtl_433 marks its meaning as unconfirmed */
    int16_t temp_dc;       /* 0.1 °C */
    uint8_t humidity;      /* % */
    uint16_t wind_raw;     /* km/h = raw / 8.333 */
    uint8_t gust_raw;      /* km/h = raw / 1.25 */
    uint16_t wind_dir_deg;
    uint16_t rain_raw;     /* mm = raw * 0.233, running total since the sensor powered up */
    int8_t uv_index;
    uint32_t light_lux;
    uint8_t counter;       /* increments with every transmission */
} rf_vevor_reading_t;

typedef enum {
    RF_VEVOR_OK,
    RF_VEVOR_SHORT,
    RF_VEVOR_CHECKSUM,
    RF_VEVOR_COUNTER,
    RF_VEVOR_NOT_WEATHER,
} rf_vevor_result_t;

rf_vevor_result_t rf_vevor_decode(const uint8_t *b, size_t len, rf_vevor_reading_t *out);
double rf_vevor_wind_kmh(uint16_t raw);
double rf_vevor_gust_kmh(uint8_t raw);
double rf_vevor_rain_mm(uint16_t raw);
/* CC1101 RSSI register to dBm (offset 74 dB, as in TI DN505). */
int rf_cc1101_rssi_dbm(uint8_t raw);
