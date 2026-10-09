#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_err.h"
#include <string.h>
static const char *TAG="network";
static void wifi_events(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if(base==WIFI_EVENT && id==WIFI_EVENT_STA_START) esp_wifi_connect();
    if(base==WIFI_EVENT && id==WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG,"STA disconnected; reconnecting (recovery AP remains available)");
        esp_wifi_connect();
    }
}
esp_err_t network_start(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,wifi_events,NULL));
    wifi_config_t sta={0}, ap={0};
    /* Provisioning is required before a real STA network can be joined.
       Never embed production Wi-Fi passwords in firmware. */
    strcpy((char*)ap.ap.ssid,"RF-Controller-Setup");
    strcpy((char*)ap.ap.password,"");
    ap.ap.ssid_len=0;
    ap.ap.authmode=WIFI_AUTH_OPEN;
    ap.ap.max_connection=2;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP,&ap));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA,&sta));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGW(TAG,"OPEN setup AP is for isolated bench provisioning only; do not deploy as-is");
    return ESP_OK;
}
