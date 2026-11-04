#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#define RF_SSID_MAX 32
#define RF_WIFI_PASSWORD_MIN 8
#define RF_WIFI_PASSWORD_MAX 64
#define RF_PEM_MAX 16383
typedef struct { bool wifi, tls; } rf_provision_status_t;
esp_err_t rf_provision_set_wifi(const char *ssid, const char *password);
/* Replaces the board's bootstrap certificate for HTTPS from the next boot. */
esp_err_t rf_provision_set_tls(const char *cert, const char *key);
/* Removes a stored certificate, so the bootstrap certificate is used again from the next boot. */
esp_err_t rf_provision_clear_tls(void);
void rf_provision_get_status(rf_provision_status_t *out);
