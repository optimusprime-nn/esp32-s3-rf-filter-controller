#include "provisioning.h"
#include "psa/crypto.h"
#include <string.h>
esp_err_t rf_password_kdf(const char *password, const uint8_t *salt, size_t salt_len, uint32_t iterations, uint8_t *out, size_t out_len) {
    if(psa_crypto_init()!=PSA_SUCCESS) return ESP_FAIL;
    psa_key_derivation_operation_t op=PSA_KEY_DERIVATION_OPERATION_INIT;
    psa_status_t s=psa_key_derivation_setup(&op,PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256));
    if(s==PSA_SUCCESS) s=psa_key_derivation_input_integer(&op,PSA_KEY_DERIVATION_INPUT_COST,iterations);
    if(s==PSA_SUCCESS) s=psa_key_derivation_input_bytes(&op,PSA_KEY_DERIVATION_INPUT_SALT,salt,salt_len);
    if(s==PSA_SUCCESS) s=psa_key_derivation_input_bytes(&op,PSA_KEY_DERIVATION_INPUT_PASSWORD,(const uint8_t*)password,strlen(password));
    if(s==PSA_SUCCESS) s=psa_key_derivation_output_bytes(&op,out,out_len);
    psa_key_derivation_abort(&op);
    return s==PSA_SUCCESS?ESP_OK:ESP_FAIL;
}
