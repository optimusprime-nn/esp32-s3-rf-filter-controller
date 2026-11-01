#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#define RF_HOSTNAME_MAX 63
#define RF_FQDN_MAX 253
#define RF_NTP_MAX 253
typedef struct {
    bool dhcp;
    char ipv4[16], netmask[16], gateway[16], dns[16];
    char hostname[RF_HOSTNAME_MAX+1];
    char fqdn[RF_FQDN_MAX+1];
    char ntp_server[RF_NTP_MAX+1];
    char cloudflare_zone[RF_FQDN_MAX+1];
    bool acme_enabled;
} rf_management_settings_t;
bool rf_settings_valid(const rf_management_settings_t *s);
esp_err_t rf_settings_load(rf_management_settings_t *out);
esp_err_t rf_settings_save(const rf_management_settings_t *in);
