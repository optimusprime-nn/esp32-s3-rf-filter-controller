#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#define RF_SSID_MAX 32
#define RF_WIFI_PASSWORD_MIN 8
#define RF_WIFI_PASSWORD_MAX 64
#define RF_DEVICE_PASSWORD_MIN 12
#define RF_DEVICE_PASSWORD_MAX 64
#define RF_PEM_MAX 16383
typedef struct { bool wifi, device_password, tls; } rf_provision_status_t;
esp_err_t rf_provision_set_wifi(const char *ssid, const char *password);
esp_err_t rf_provision_set_device_password(const char *password);
esp_err_t rf_provision_verify_device_password(const char *password, bool *match);
esp_err_t rf_provision_set_tls(const char *cert, const char *key);
void rf_provision_get_status(rf_provision_status_t *out);
/* PBKDF2-HMAC-SHA256; a separate unit so host tests can replace the crypto library. */
esp_err_t rf_password_kdf(const char *password, const uint8_t *salt, size_t salt_len, uint32_t iterations, uint8_t *out, size_t out_len);
