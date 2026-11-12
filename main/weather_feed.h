#pragma once
#include <stddef.h>
#include "esp_err.h"
/* Read-only TCP feed of weather station readings for other devices on the LAN, such as
   the ESP32-P4 ADS-B display. Each client gets one line of JSON per reading, the latest
   reading as soon as it connects, and a repeat every 30 s while no new reading arrives.
   Clients can't send commands: anything they send is discarded. */
#define RF_WEATHER_FEED_MAX_CLIENTS 4
esp_err_t rf_weather_feed_start(void);
/* The listening port, or 0 when the feed is off. */
int rf_weather_feed_port(void);
size_t rf_weather_feed_clients(void);
