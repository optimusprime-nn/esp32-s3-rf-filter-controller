/* Host tests for settings validation. The lwIP parser is stubbed, so valid_ip's own checks are what runs here. */
#include "management_settings.h"
#include "nvs.h"
#include "lwip/ip4_addr.h"
#include <stdio.h>
#include <string.h>
static unsigned char store[sizeof(rf_management_settings_t)+8];
static size_t store_len;
int ip4addr_aton(const char *s, ip4_addr_t *addr) {
    return inet_pton(AF_INET,s,&addr->addr)==1;
}
esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *h) { *h=1; return ESP_OK; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len) {
    if(!store_len) return ESP_ERR_NVS_NOT_FOUND;
    memcpy(out,store,store_len<*len?store_len:*len);
    *len=store_len;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *in, size_t len) {
    memcpy(store,in,len); store_len=len; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { return ESP_OK; }
void nvs_close(nvs_handle_t h) {}
static int failures;
#define CHECK(x) do { if(!(x)) { printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); failures++; } } while(0)
static rf_management_settings_t static_settings(const char *ip,const char *mask,const char *gw) {
    rf_management_settings_t s={.dhcp=false,.hostname="rf",.fqdn="rf.example.com",.ntp_server="pool.ntp.org",.dns="192.168.1.1"};
    strcpy(s.ipv4,ip); strcpy(s.netmask,mask); strcpy(s.gateway,gw);
    return s;
}
int main(void) {
    rf_management_settings_t s;
    CHECK(rf_settings_load(&s)==ESP_OK && s.dhcp && rf_settings_valid(&s));
    s=static_settings("192.168.1.10","255.255.255.0","192.168.1.1");
    CHECK(rf_settings_valid(&s));
    s=static_settings("10","255.255.255.0","192.168.1.1");
    CHECK(!rf_settings_valid(&s));
    s=static_settings("10.1","255.0.0.0","10.0.0.1");
    CHECK(!rf_settings_valid(&s));
    s=static_settings("0x0a.0.0.5","255.0.0.0","10.0.0.1");
    CHECK(!rf_settings_valid(&s));
    s=static_settings("192.168.1.10","255.0.255.0","192.168.1.1");
    CHECK(!rf_settings_valid(&s));
    s=static_settings("192.168.1.0","255.255.255.0","192.168.1.1");
    CHECK(!rf_settings_valid(&s));
    s=static_settings("192.168.1.255","255.255.255.0","192.168.1.1");
    CHECK(!rf_settings_valid(&s));
    s=static_settings("192.168.1.10","255.255.255.0","192.168.2.1");
    CHECK(!rf_settings_valid(&s));
    s=static_settings("192.168.1.10","255.255.255.0","192.168.1.10");
    CHECK(!rf_settings_valid(&s));
    s=static_settings("192.168.1.10","255.255.255.0","192.168.1.1");
    strcpy(s.hostname,"has.dot");
    CHECK(!rf_settings_valid(&s));
    strcpy(s.hostname,"-bad");
    CHECK(!rf_settings_valid(&s));
    strcpy(s.hostname,"rf");
    s.acme_enabled=true;
    CHECK(!rf_settings_valid(&s));
    strcpy(s.cloudflare_zone,"example.com");
    CHECK(rf_settings_valid(&s));
    CHECK(rf_settings_save(&s)==ESP_OK);
    rf_management_settings_t out;
    CHECK(rf_settings_load(&out)==ESP_OK && !memcmp(&out,&s,sizeof(s)));
    s.hostname[0]='\0';
    CHECK(rf_settings_save(&s)==ESP_ERR_INVALID_ARG);
    memset(store,'x',sizeof(rf_management_settings_t));
    CHECK(rf_settings_load(&out)==ESP_ERR_INVALID_STATE);
    store_len=sizeof(rf_management_settings_t)-1;
    CHECK(rf_settings_load(&out)==ESP_ERR_INVALID_STATE);
    printf("%s\n",failures?"FAILED":"PASSED");
    return failures!=0;
}
