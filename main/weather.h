#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "weather_decode.h"
#include "weather_history.h"

typedef enum { RF_WEATHER_DISABLED, RF_WEATHER_MISSING, RF_WEATHER_RUNNING } rf_weather_receiver_t;

#define RF_WEATHER_MAX_HEARD 4

typedef struct {
    uint16_t id;
    int rssi_dbm;
    int64_t age_ms;
} rf_weather_heard_t;

typedef struct {
    rf_weather_receiver_t receiver;
    uint8_t chip_version;
    uint32_t frequency_khz;
    int32_t sensor_id_filter;       /* -1 accepts any station */
    uint32_t packets_ok;            /* new readings from the accepted station */
    uint32_t packets_rejected;      /* failed checks; includes noise that matched the sync word */
    uint32_t packets_ignored;       /* valid packets from other stations */
    bool have_reading;
    rf_vevor_reading_t last;
    int rssi_dbm;
    int64_t age_ms;                 /* time since the last reading, -1 for none */
    size_t heard_count;
    rf_weather_heard_t heard[RF_WEATHER_MAX_HEARD];   /* every station heard recently, accepted or not */
    bool invert;                    /* FSK sense flipped, from menuconfig or set at runtime */
    bool have_rssi;                 /* the two below are valid */
    int rssi_floor_dbm;             /* quietest one-second average in the last minute */
    int rssi_peak_dbm;              /* strongest sample in the last minute, sampled every 10 ms */
} rf_weather_status_t;

/* Seconds without a packet before a reading is reported as stale; the station sends every 20 s. */
#define RF_WEATHER_STALE_S 90

/* Probes the CC1101 and starts receiving. A missing chip is not an error: the status says so. */
esp_err_t rf_weather_start(void);
void rf_weather_status(rf_weather_status_t *out);
/* Accepts only this station ID from now on, or any with -1. Saved for the next boot. */
esp_err_t rf_weather_set_station(int32_t id);
/* Retunes the receiver (779 to 928 MHz) and sets the FSK sense. Saved for the next boot,
   overriding menuconfig. ESP_ERR_INVALID_STATE if the receiver isn't running. */
esp_err_t rf_weather_set_radio(uint32_t khz, bool invert);
/* History, one sample a minute while readings arrive; cleared at reboot. */
void rf_weather_history_span(uint32_t *oldest, uint32_t *end, size_t *capacity);
size_t rf_weather_history_copy(uint32_t seq, rf_weather_sample_t *out, size_t max, uint32_t *next);
const char *rf_weather_receiver_name(rf_weather_receiver_t r);

/* The last packets that failed decoding, kept to tell a mis-set receiver from noise. */
#define RF_WEATHER_MAX_REJECTS 8
#define RF_WEATHER_REJECT_OVERFLOW 0xFF   /* the FIFO overflowed: no bytes */
typedef struct {
    int64_t age_ms;
    uint8_t reason;                       /* an rf_vevor_result_t, or RF_WEATHER_REJECT_OVERFLOW */
    int rssi_dbm;
    uint8_t bytes[RF_VEVOR_PACKET_LEN];   /* as received, before any inversion */
} rf_weather_reject_t;
/* Copies them newest first; returns how many. */
size_t rf_weather_rejects(rf_weather_reject_t *out, size_t max);
const char *rf_weather_reject_name(uint8_t reason);
