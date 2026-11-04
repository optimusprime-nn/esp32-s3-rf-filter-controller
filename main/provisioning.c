#include "provisioning.h"
#include "nvs.h"
#include <string.h>
static bool printable(const char *s) {
    for(;*s;s++) if((unsigned char)*s<0x20 || *s==0x7f) return false;
    return true;
}
static bool hex_key(const char *s) {
    for(;*s;s++) if(!strchr("0123456789abcdefABCDEF",*s)) return false;
    return true;
}
static bool wifi_valid(const char *ssid, const char *password) {
    if(!ssid || !password) return false;
    size_t sn=strnlen(ssid,RF_SSID_MAX+1),pn=strnlen(password,RF_WIFI_PASSWORD_MAX+1);
    if(!sn || sn>RF_SSID_MAX || pn<RF_WIFI_PASSWORD_MIN || pn>RF_WIFI_PASSWORD_MAX) return false;
    if(!printable(ssid) || !printable(password)) return false;
    /* 64 characters is the raw PSK form, which must be hex; passphrases stop at 63. */
    return pn<RF_WIFI_PASSWORD_MAX || hex_key(password);
}
esp_err_t rf_provision_set_wifi(const char *ssid, const char *password) {
    if(!wifi_valid(ssid,password)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t e=nvs_open("wifi",NVS_READWRITE,&h);
    if(e!=ESP_OK) return e;
    e=nvs_set_str(h,"ssid",ssid);
    if(e==ESP_OK) e=nvs_set_str(h,"password",password);
    if(e==ESP_OK) e=nvs_commit(h);
    nvs_close(h);
    return e;
}
static bool pem_valid(const char *pem) {
    if(!pem) return false;
    size_t n=strnlen(pem,RF_PEM_MAX+1);
    /* web.c rejects anything under 32 bytes, counted with the terminator. */
    return n>=31 && n<=RF_PEM_MAX && !strncmp(pem,"-----BEGIN ",11) && strstr(pem,"-----END ");
}
esp_err_t rf_provision_set_tls(const char *cert, const char *key) {
    if(!pem_valid(cert) || !pem_valid(key)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t e=nvs_open("tls",NVS_READWRITE,&h);
    if(e!=ESP_OK) return e;
    /* Stored with the terminator: mbedTLS and load_pem both require NUL-terminated PEM. */
    e=nvs_set_blob(h,"cert",cert,strlen(cert)+1);
    if(e==ESP_OK) e=nvs_set_blob(h,"key",key,strlen(key)+1);
    if(e==ESP_OK) e=nvs_commit(h);
    nvs_close(h);
    return e;
}
esp_err_t rf_provision_clear_tls(void) {
    nvs_handle_t h;
    esp_err_t e=nvs_open("tls",NVS_READWRITE,&h);
    if(e!=ESP_OK) return e;
    e=nvs_erase_all(h);
    if(e==ESP_OK) e=nvs_commit(h);
    nvs_close(h);
    return e;
}
static bool str_present(nvs_handle_t h, const char *key) {
    size_t n=0;
    return nvs_get_str(h,key,NULL,&n)==ESP_OK && n>1;
}
static bool blob_present(nvs_handle_t h, const char *key) {
    size_t n=0;
    return nvs_get_blob(h,key,NULL,&n)==ESP_OK && n>0;
}
void rf_provision_get_status(rf_provision_status_t *out) {
    nvs_handle_t h;
    *out=(rf_provision_status_t){0};
    if(nvs_open("wifi",NVS_READONLY,&h)==ESP_OK) {
        out->wifi=str_present(h,"ssid") && str_present(h,"password");
        nvs_close(h);
    }
    if(nvs_open("tls",NVS_READONLY,&h)==ESP_OK) {
        out->tls=blob_present(h,"cert") && blob_present(h,"key");
        nvs_close(h);
    }
}
