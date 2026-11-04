#pragma once
#include <stddef.h>
#include "esp_err.h"
/* Per-board self-signed P-256 certificate, created at first boot and kept in NVS. Firmware images carry no key. */
esp_err_t rf_bootstrap_tls_load(char **cert_pem, size_t *cert_len, char **key_pem, size_t *key_len);
/* SHA-256 of the certificate's DER, as 64 upper-case hex characters. */
esp_err_t rf_bootstrap_tls_sha256(char out_hex[65]);
