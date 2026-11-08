#pragma once
/* ACME protocol pieces copied from esp32p4_nts_time_server (see acme_core.c). */
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define ACME_URL_MAX_LENGTH       384U
#define ACME_THUMBPRINT_LENGTH     44U
#define ACME_DNS_NAME_MAX_LENGTH    253U
#define ACME_CHALLENGE_TOKEN_MAX_LENGTH 255U
#define ACME_DNS01_VALUE_LENGTH      44U

typedef struct {
    bool reachable;
    int http_status;
    char new_nonce_url[ACME_URL_MAX_LENGTH];
    char new_account_url[ACME_URL_MAX_LENGTH];
    char new_order_url[ACME_URL_MAX_LENGTH];
} acme_directory_status_t;

typedef struct {
    bool key_present;
    bool registered;
    int http_status;
    char account_url[ACME_URL_MAX_LENGTH];
    char jwk_thumbprint[ACME_THUMBPRINT_LENGTH];
} acme_account_status_t;

typedef struct {
    bool discovered;
    int new_order_http_status;
    int authorization_http_status;
    char identifier[ACME_DNS_NAME_MAX_LENGTH + 1U];
    char order_url[ACME_URL_MAX_LENGTH];
    char authorization_url[ACME_URL_MAX_LENGTH];
    char finalize_url[ACME_URL_MAX_LENGTH];
    char challenge_url[ACME_URL_MAX_LENGTH];
    char challenge_token[ACME_CHALLENGE_TOKEN_MAX_LENGTH + 1U];
    char challenge_status[24];
    char dns01_record_name[ACME_DNS_NAME_MAX_LENGTH + 32U];
    char dns01_value[ACME_DNS01_VALUE_LENGTH];
} acme_order_discovery_t;

typedef struct {
    bool triggered;
    bool valid;
    int trigger_http_status;
    int poll_http_status;
    unsigned poll_count;
    char authorization_status[24];
    char challenge_status[24];
} acme_challenge_validation_t;

typedef struct {
    bool finalized;
    bool certificate_retrieved;
    bool stored;
    int finalize_http_status;
    int order_poll_http_status;
    int certificate_http_status;
    unsigned order_poll_count;
    size_t certificate_pem_length;
    char identifier[ACME_DNS_NAME_MAX_LENGTH + 1U];
    char order_status[24];
    char certificate_url[ACME_URL_MAX_LENGTH];
} acme_certificate_issue_status_t;

esp_err_t acme_core_provision_account(bool production, acme_account_status_t *out);
esp_err_t acme_core_discover_order(bool production, const char *hostname, acme_order_discovery_t *out);
/* Triggers the DNS-01 challenge (the TXT record must already be published) and waits for the result. */
esp_err_t acme_core_validate_dns01(bool production, const acme_order_discovery_t *order, acme_challenge_validation_t *out);
/* Makes a new key and CSR, finalizes the order, downloads the chain and stores it with acme_core_store_certificate. */
esp_err_t acme_core_finalize_order(bool production, const acme_order_discovery_t *order, acme_certificate_issue_status_t *out);
bool acme_core_dns_name_valid(const char *name);
/* Implemented in acme.c: keeps a checked certificate chain and its key. */
esp_err_t acme_core_store_certificate(bool production, const char *hostname, const char *chain_pem, const char *key_pem);
