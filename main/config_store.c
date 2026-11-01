#include "nvs.h"
#include "nvs_flash.h"
#include "esp_err.h"
#include <stdint.h>
esp_err_t config_store_init(void) {
    esp_err_t err=nvs_flash_init();
    if(err==ESP_ERR_NVS_NO_FREE_PAGES || err==ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* Do not erase credentials/configuration automatically. */
        return err;
    }
    return err;
}
esp_err_t config_store_load_port(uint8_t *port) {
    if(!port) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err=nvs_open("rfconfig",NVS_READONLY,&h);
    if(err==ESP_ERR_NVS_NOT_FOUND) { *port=1; return ESP_OK; }
    if(err!=ESP_OK) return err;
    err=nvs_get_u8(h,"port",port);
    nvs_close(h);
    if(err==ESP_ERR_NVS_NOT_FOUND) { *port=1; return ESP_OK; }
    if(err==ESP_OK && (*port<1 || *port>4)) return ESP_ERR_INVALID_STATE;
    return err;
}
esp_err_t config_store_save_port(uint8_t port) {
    if(port<1 || port>4) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err=nvs_open("rfconfig",NVS_READWRITE,&h);
    if(err!=ESP_OK) return err;
    err=nvs_set_u8(h,"port",port);
    if(err==ESP_OK) err=nvs_commit(h);
    nvs_close(h);
    return err;
}
