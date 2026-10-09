#pragma once
#include "esp_err.h"
esp_err_t rf_time_service_start(const char *server);
bool rf_time_service_is_synchronized(void);
