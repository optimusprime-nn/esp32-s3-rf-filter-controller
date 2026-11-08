#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
/* Let's Encrypt certificates by DNS-01 through Cloudflare, as on esp32p4_nts_time_server.
   The certificate name is the settings' fqdn, the zone is cloudflare_zone, and the API
   token is kept in NVS. An issued certificate goes into the same store as one set by
   hand (PUT /api/v1/config/tls), and HTTPS switches to it without a reboot. */
typedef enum { RF_ACME_IDLE, RF_ACME_RUNNING, RF_ACME_DONE, RF_ACME_FAILED } rf_acme_state_t;
typedef struct {
    rf_acme_state_t state;
    bool production;            /* of the running or last run */
    char stage[24];             /* account, order, dns, challenge, finalize, cleanup, done */
    char error[120];
    int64_t started, finished;  /* Unix seconds, 0 when not yet */
    bool token_present;
    char zone_id[33];
    /* The certificate HTTPS is configured with, if one is stored. */
    bool cert_present, cert_from_acme, cert_production;
    char cert_hostname[254];
    char cert_issuer[96];
    char cert_valid_from[24], cert_valid_to[24];
    int64_t cert_expires;       /* Unix seconds */
    int cert_days_left;
    char cert_sha256[65];
} rf_acme_status_t;
esp_err_t rf_acme_start(void);
/* Checks the token can see zone_name (from this board's network) and stores both. */
esp_err_t rf_acme_set_cloudflare(const char *token, const char *zone_name, int *http_status);
esp_err_t rf_acme_clear_cloudflare(void);
/* Starts issuance in the background. ESP_ERR_INVALID_STATE while one runs or when the
   name, zone or token is missing; ESP_ERR_TIMEOUT until the clock is synchronized. */
esp_err_t rf_acme_issue(bool production);
void rf_acme_status(rf_acme_status_t *out);
/* Call when a certificate is stored or removed by hand, so it isn't reported or renewed as issued. */
void rf_acme_forget_certificate(void);
const char *rf_acme_state_name(rf_acme_state_t s);
