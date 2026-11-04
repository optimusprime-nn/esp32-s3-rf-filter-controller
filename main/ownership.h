#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
/* Management ownership, matching esp32p4_nts_time_server: an unclaimed board accepts one CA
 * through PUT /api/v1/provisioning/claim with a one-time code printed on the serial console;
 * a claimed board accepts management only from client certificates signed by that CA. */
#define RF_OWNERSHIP_CODE_LEN 32
#define RF_OWNERSHIP_CA_MAX 6144
esp_err_t rf_ownership_init(void);
bool rf_ownership_claimed(void);
/* The enrollment code, or "" once claimed. */
const char *rf_ownership_code(void);
bool rf_ownership_code_matches(const char *candidate);
/* Validates the CA (it must parse and be allowed to sign certificates) and stores it. */
esp_err_t rf_ownership_claim(const char *code, const char *ca_pem, size_t ca_len);
/* Allocates the stored CA as NUL-terminated PEM; *len counts the terminator. */
esp_err_t rf_ownership_load_ca(char **pem, size_t *len);
