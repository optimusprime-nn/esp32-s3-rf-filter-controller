#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs.h"
#include "management_settings.h"
#include "lwip/ip4_addr.h"
#include <string.h>
static const char *TAG="network";
static esp_netif_t *sta_netif;
static void wifi_events(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if(base==WIFI_EVENT && id==WIFI_EVENT_STA_START) esp_wifi_connect();
    if(base==WIFI_EVENT && id==WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG,"STA disconnected; reconnecting");
        esp_wifi_connect();
    }
    if(base==IP_EVENT && id==IP_EVENT_STA_GOT_IP) ESP_LOGI(TAG,"Station received IPv4 address");
}
static esp_err_t static_address(const rf_management_settings_t *s) {
    if(s->dhcp) return ESP_OK;
    esp_netif_ip_info_t ip={0};
    ip4addr_aton(s->ipv4,(ip4_addr_t*)&ip.ip);
    ip4addr_aton(s->netmask,(ip4_addr_t*)&ip.netmask);
    ip4addr_aton(s->gateway,(ip4_addr_t*)&ip.gw);
    esp_err_t e=esp_netif_dhcpc_stop(sta_netif);
    if(e!=ESP_OK && e!=ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) return e;
    e=esp_netif_set_ip_info(sta_netif,&ip);
    if(e!=ESP_OK) return e;
    esp_netif_dns_info_t dns={.ip.type=ESP_IPADDR_TYPE_V4};
    ip4addr_aton(s->dns,(ip4_addr_t*)&dns.ip.u_addr.ip4);
    return esp_netif_set_dns_info(sta_netif,ESP_NETIF_DNS_MAIN,&dns);
}
esp_err_t network_start(const rf_management_settings_t *settings) {
    if(!settings || !rf_settings_valid(settings)) return ESP_ERR_INVALID_ARG;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_netif=esp_netif_create_default_wifi_sta();
    if(!sta_netif) return ESP_ERR_NO_MEM;
    ESP_ERROR_CHECK(esp_netif_set_hostname(sta_netif,settings->hostname));
    ESP_ERROR_CHECK(static_address(settings));
    wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,wifi_events,NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,wifi_events,NULL));
    /* Credentials must be provisioned locally in namespace 'wifi', not built into firmware.
       No open recovery AP is started by default. */
    char ssid[33]={0},password[65]={0};
    size_t sn=sizeof(ssid),pn=sizeof(password);
    nvs_handle_t h;
    esp_err_t e=nvs_open("wifi",NVS_READONLY,&h);
    if(e!=ESP_OK) { ESP_LOGW(TAG,"Wi-Fi credentials missing; network disabled"); return ESP_ERR_NOT_FOUND; }
    esp_err_t a=nvs_get_str(h,"ssid",ssid,&sn);
    esp_err_t b=nvs_get_str(h,"password",password,&pn);
    nvs_close(h);
    if(a!=ESP_OK || b!=ESP_OK || !ssid[0]) return ESP_ERR_NOT_FOUND;
    wifi_config_t sta={0};
    memcpy(sta.sta.ssid,ssid,strlen(ssid));
    memcpy(sta.sta.password,password,strlen(password));
    sta.sta.threshold.authmode=WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA,&sta));
    memset(password,0,sizeof(password));
    return esp_wifi_start();
}
