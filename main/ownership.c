/* Ported from esp32p4_nts_time_server (management_ownership.c); same NVS layout and log lines,
 * which its setup tool reads from the serial console. */
#include "ownership.h"
#include "console_line.h"
#include "der_util.h"
#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/x509_crt.h"
#include "nvs.h"
#include <stdlib.h>
#include <string.h>
static const char *TAG="OWNERSHIP";
#define NVS_NS "mgmt_owner"
static bool initialized,claimed;
static char code[RF_OWNERSHIP_CODE_LEN+1];
static void make_code(void) {
    uint8_t raw[RF_OWNERSHIP_CODE_LEN/2];
    esp_fill_random(raw,sizeof(raw));
    rf_hex_upper(raw,sizeof(raw),code);
    rf_secure_zero(raw,sizeof(raw));
    ESP_LOGW(TAG,"Device is UNPROVISIONED");
    ESP_LOGW(TAG,"One-time management enrollment code: %s",code);
    ESP_LOGW(TAG,"Enrollment is permitted only until a management CA is claimed");
}
static esp_err_t validate_ca(const char *pem, size_t len) {
    if(!pem || len<64 || len>RF_OWNERSHIP_CA_MAX || memchr(pem,'\0',len)) return ESP_ERR_INVALID_ARG;
    char *t=calloc(1,len+1);
    if(!t) return ESP_ERR_NO_MEM;
    memcpy(t,pem,len);
    bool is_ca=false;
    mbedtls_x509_crt cert;
    mbedtls_x509_crt_init(&cert);
    int rc=strstr(t,"-----BEGIN CERTIFICATE-----")?mbedtls_x509_crt_parse(&cert,(const unsigned char*)t,len+1):-1;
    for(mbedtls_x509_crt *p=&cert;!rc && p;p=p->next) if(!mbedtls_x509_crt_check_key_usage(p,MBEDTLS_X509_KU_KEY_CERT_SIGN)) { is_ca=true; break; }
    mbedtls_x509_crt_free(&cert);
    free(t);
    return !rc && is_ca?ESP_OK:ESP_ERR_INVALID_ARG;
}
esp_err_t rf_ownership_init(void) {
    if(initialized) return ESP_OK;
    nvs_handle_t h;
    esp_err_t e=nvs_open(NVS_NS,NVS_READONLY,&h);
    claimed=false;
    if(e==ESP_OK) {
        uint8_t flag=0;
        size_t n=0;
        if(nvs_get_u8(h,"claimed",&flag)==ESP_OK && flag==1) {
            /* A claimed flag without a usable CA is corrupt: stay unmanaged rather than reopen enrollment. */
            if(nvs_get_str(h,"client_ca",NULL,&n)!=ESP_OK || n<2 || n>RF_OWNERSHIP_CA_MAX+1) e=ESP_ERR_INVALID_STATE;
            else claimed=true;
        }
        nvs_close(h);
    } else if(e==ESP_ERR_NVS_NOT_FOUND) e=ESP_OK;
    if(e!=ESP_OK) return e;
    if(claimed) ESP_LOGI(TAG,"Management ownership is provisioned; mTLS client authentication required");
    else make_code();
    initialized=true;
    return ESP_OK;
}
bool rf_ownership_claimed(void) {
    return initialized && claimed;
}
const char *rf_ownership_code(void) {
    return initialized && !claimed?code:"";
}
bool rf_ownership_code_matches(const char *candidate) {
    if(!initialized || claimed || !candidate || strlen(candidate)!=RF_OWNERSHIP_CODE_LEN) return false;
    unsigned char diff=0;
    for(size_t i=0;i<RF_OWNERSHIP_CODE_LEN;i++) diff|=(unsigned char)(candidate[i]^code[i]);
    return !diff;
}
esp_err_t rf_ownership_claim(const char *candidate, const char *ca_pem, size_t ca_len) {
    if(!initialized || claimed) return ESP_ERR_INVALID_STATE;
    if(!rf_ownership_code_matches(candidate)) return ESP_ERR_INVALID_ARG;
    esp_err_t e=validate_ca(ca_pem,ca_len);
    if(e!=ESP_OK) return e;
    char *t=calloc(1,ca_len+1);
    if(!t) return ESP_ERR_NO_MEM;
    memcpy(t,ca_pem,ca_len);
    nvs_handle_t h;
    e=nvs_open(NVS_NS,NVS_READWRITE,&h);
    if(e==ESP_OK) {
        e=nvs_set_str(h,"client_ca",t);
        if(e==ESP_OK) e=nvs_set_u8(h,"claimed",1);
        if(e==ESP_OK) e=nvs_commit(h);
        nvs_close(h);
    }
    free(t);
    if(e==ESP_OK) {
        claimed=true;
        rf_secure_zero(code,sizeof(code));
        ESP_LOGI(TAG,"Management ownership claimed; enrollment closed until the ownership state is erased");
    }
    return e;
}
esp_err_t rf_ownership_load_ca(char **pem, size_t *len) {
    *pem=NULL; *len=0;
    if(!rf_ownership_claimed()) return ESP_ERR_INVALID_STATE;
    nvs_handle_t h;
    esp_err_t e=nvs_open(NVS_NS,NVS_READONLY,&h);
    if(e!=ESP_OK) return e;
    size_t n=0;
    e=nvs_get_str(h,"client_ca",NULL,&n);
    char *buf=e==ESP_OK?calloc(1,n):NULL;
    if(e==ESP_OK && !buf) e=ESP_ERR_NO_MEM;
    if(e==ESP_OK) e=nvs_get_str(h,"client_ca",buf,&n);
    nvs_close(h);
    if(e!=ESP_OK) { free(buf); return e; }
    *pem=buf; *len=n;
    return ESP_OK;
}
