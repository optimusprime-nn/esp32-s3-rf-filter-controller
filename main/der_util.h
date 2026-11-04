#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
/* Minimal DER and PEM helpers for the bootstrap certificate, ported from esp32p4_nts_time_server. */
/* out must not overlap content. */
esp_err_t rf_der_wrap(uint8_t tag, const uint8_t *content, size_t content_len, uint8_t *out, size_t out_size, size_t *out_len);
esp_err_t rf_der_concat(const uint8_t *a, size_t al, const uint8_t *b, size_t bl, uint8_t *out, size_t out_size, size_t *out_len);
/* Converts a raw r||s P-256 signature to a DER ECDSA-Sig-Value. */
esp_err_t rf_ecdsa_raw_to_der(const uint8_t raw[64], uint8_t *out, size_t out_size, size_t *out_len);
/* Allocates a NUL-terminated PEM block; *out_len counts the terminator. */
esp_err_t rf_pem_encode(const char *label, const uint8_t *der, size_t der_len, char **out_pem, size_t *out_len);
void rf_hex_upper(const uint8_t *in, size_t n, char *out);
