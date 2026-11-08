#pragma once
#include <stdbool.h>
#include "esp_err.h"
/* Polls both servers; lwIP SNTP uses the first that answers. */
esp_err_t rf_time_service_start(const char *primary, const char *secondary);
bool rf_time_service_is_synchronized(void);
