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
    if(!str || strnlen(str,16)>=16 || strspn(str,"0123456789.")!=strlen(str)) return false;
    /* ip4addr_aton also accepts short forms such as "10" or "10.1". */
    int dots=0;
    for(const char *p=str;*p;p++) dots+=*p=='.';
    return dots==3 && ip4addr_aton(str,&addr);
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
       !valid_dns(s->ntp_servers[0],RF_NTP_MAX,true) || !valid_dns(s->ntp_servers[1],RF_NTP_MAX,true)) return false;
    if(s->acme_enabled && (!strchr(s->fqdn,'.') || !valid_dns(s->cloudflare_zone,RF_FQDN_MAX,true))) return false;
    return s->dhcp || valid_static(s);
}
static const rf_management_settings_t defaults={
    .dhcp=true,.hostname="rf-controller",.fqdn="rf-controller.local",.ntp_servers={"time.cloudflare.com","time.google.com"}
};
/* Keeps the one stored server first and adds a default second one that differs from it. */
static void from_v1(const rf_management_settings_v1_t *v1, rf_management_settings_t *out) {
    *out=defaults;
    out->dhcp=v1->dhcp;
    memcpy(out->ipv4,v1->ipv4,sizeof(out->ipv4)); memcpy(out->netmask,v1->netmask,sizeof(out->netmask));
    memcpy(out->gateway,v1->gateway,sizeof(out->gateway)); memcpy(out->dns,v1->dns,sizeof(out->dns));
    memcpy(out->hostname,v1->hostname,sizeof(out->hostname)); memcpy(out->fqdn,v1->fqdn,sizeof(out->fqdn));
    memcpy(out->ntp_servers[0],v1->ntp_server,sizeof(out->ntp_servers[0]));
    strcpy(out->ntp_servers[1],strcmp(v1->ntp_server,defaults.ntp_servers[0])?defaults.ntp_servers[0]:defaults.ntp_servers[1]);
    memcpy(out->cloudflare_zone,v1->cloudflare_zone,sizeof(out->cloudflare_zone));
    out->acme_enabled=v1->acme_enabled;
}
esp_err_t rf_settings_load(rf_management_settings_t *out) {
    if(!out) return ESP_ERR_INVALID_ARG;
    *out=defaults;
    nvs_handle_t h; esp_err_t e=nvs_open("mgmtcfg",NVS_READONLY,&h);
    if(e==ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if(e!=ESP_OK) return e;
    size_t n=sizeof(*out); e=nvs_get_blob(h,"settings",out,&n); nvs_close(h);
    if(e==ESP_ERR_NVS_NOT_FOUND) { *out=defaults; return ESP_OK; }
    if(e==ESP_OK && n==sizeof(rf_management_settings_v1_t)) {
        rf_management_settings_v1_t v1;
        memcpy(&v1,out,sizeof(v1));
        from_v1(&v1,out);
        n=sizeof(*out);
    }
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
