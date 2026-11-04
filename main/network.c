#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "nvs.h"
#include "management_settings.h"
#include "lwip/ip4_addr.h"
#include <string.h>
static const char *TAG="network";
#define RECONNECT_MIN_MS 1000
#define RECONNECT_MAX_MS 60000
static esp_netif_t *sta_netif;
static bool static_ipv4;
static esp_timer_handle_t reconnect_timer;
static uint32_t reconnect_ms=RECONNECT_MIN_MS;
static void reconnect_expired(void *arg) {
    esp_wifi_connect();
}
static void wifi_events(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if(base==WIFI_EVENT && id==WIFI_EVENT_STA_START) esp_wifi_connect();
    if(base==WIFI_EVENT && id==WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG,"STA disconnected; reconnecting in %lu ms",(unsigned long)reconnect_ms);
        esp_timer_start_once(reconnect_timer,reconnect_ms*1000ULL);
        reconnect_ms=reconnect_ms*2>RECONNECT_MAX_MS?RECONNECT_MAX_MS:reconnect_ms*2;
    }
    if(base==IP_EVENT && id==IP_EVENT_STA_GOT_IP) {
        reconnect_ms=RECONNECT_MIN_MS;
        const ip_event_got_ip_t *got=data;
        char ip[16];
        esp_ip4addr_ntoa(&got->ip_info.ip,ip,sizeof(ip));
        /* Same wording as esp32p4_nts_time_server; its setup tool reads this line. */
        ESP_LOGI(TAG,"%s IPv4 acquired: %s",static_ipv4?"Static":"DHCP",ip);
    }
}
static esp_err_t static_address(const rf_management_settings_t *s) {
    static_ipv4=!s->dhcp;
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
esp_err_t rf_network_get_ip(char *out, size_t len) {
    esp_netif_ip_info_t ip;
    if(!sta_netif || esp_netif_get_ip_info(sta_netif,&ip)!=ESP_OK || !ip.ip.addr) return ESP_ERR_INVALID_STATE;
    return esp_ip4addr_ntoa(&ip.ip,out,(int)len)?ESP_OK:ESP_ERR_INVALID_SIZE;
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
    /* Credentials live in the "wifi" namespace; do not let the driver keep a second copy in flash. */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    const esp_timer_create_args_t timer={.callback=reconnect_expired,.name="wifi_reconnect"};
    ESP_ERROR_CHECK(esp_timer_create(&timer,&reconnect_timer));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,wifi_events,NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,wifi_events,NULL));
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
    memset(&sta,0,sizeof(sta));
    return esp_wifi_start();
}
