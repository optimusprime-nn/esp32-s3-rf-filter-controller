#include "management_settings.h"
#include "nvs.h"
#include "lwip/ip4_addr.h"
#include <string.h>
#include <ctype.h>
static bool valid_dns(const char *s, size_t max, bool dots) {
    size_t n=strnlen(s,max+1);
    if(!n || n>max || s[0]=='.' || s[n-1]=='.') return false;
    size_t label=0;
    for(size_t i=0;i<n;i++) {
        char c=s[i];
        if(c=='.') { if(!dots || label==0 || s[i-1]=='-') return false; label=0; continue; }
        if(!(isalnum((unsigned char)c)||c=='-') || (label==0 && c=='-')) return false;
        if(++label>63) return false;
    }
    return s[n-1]!='-';
}
static bool valid_ip(const char *str) {
    ip4_addr_t addr;
    return str && strnlen(str,16)<16 && ip4addr_aton(str,&addr);
}
static bool valid_static(const rf_management_settings_t *s) {
    if(!valid_ip(s->ipv4)||!valid_ip(s->netmask)||!valid_ip(s->gateway)||!valid_ip(s->dns)) return false;
    ip4_addr_t ip,mask,gw;
    ip4addr_aton(s->ipv4,&ip); ip4addr_aton(s->netmask,&mask); ip4addr_aton(s->gateway,&gw);
    uint32_t m=lwip_ntohl(mask.addr), a=lwip_ntohl(ip.addr), g=lwip_ntohl(gw.addr);
    if(!m || ((~m)&((~m)+1))!=0 || (a&~m)==0 || (a&~m)==(~m)) return false;
    if((a&m)!=(g&m) || a==g) return false;
    return true;
}
bool rf_settings_valid(const rf_management_settings_t *s) {
    if(!s || !valid_dns(s->hostname,RF_HOSTNAME_MAX,false) ||
       !valid_dns(s->fqdn,RF_FQDN_MAX,true) ||
       !valid_dns(s->ntp_server,RF_NTP_MAX,true)) return false;
    if(s->acme_enabled && (!strchr(s->fqdn,'.') || !valid_dns(s->cloudflare_zone,RF_FQDN_MAX,true))) return false;
    return s->dhcp || valid_static(s);
}
static const rf_management_settings_t defaults={
    .dhcp=true,.hostname="rf-controller",.fqdn="rf-controller.local",.ntp_server="pool.ntp.org"
};
esp_err_t rf_settings_load(rf_management_settings_t *out) {
    if(!out) return ESP_ERR_INVALID_ARG;
    *out=defaults;
    nvs_handle_t h; esp_err_t e=nvs_open("mgmtcfg",NVS_READONLY,&h);
    if(e==ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if(e!=ESP_OK) return e;
    size_t n=sizeof(*out); e=nvs_get_blob(h,"settings",out,&n); nvs_close(h);
    if(e==ESP_ERR_NVS_NOT_FOUND) { *out=defaults; return ESP_OK; }
    if(e!=ESP_OK || n!=sizeof(*out) || !rf_settings_valid(out)) { *out=defaults; return ESP_ERR_INVALID_STATE; }
    return ESP_OK;
}
esp_err_t rf_settings_save(const rf_management_settings_t *in) {
    if(!rf_settings_valid(in)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h; esp_err_t e=nvs_open("mgmtcfg",NVS_READWRITE,&h);
    if(e!=ESP_OK) return e;
    e=nvs_set_blob(h,"settings",in,sizeof(*in));
    if(e==ESP_OK) e=nvs_commit(h);
    nvs_close(h); return e;
}
