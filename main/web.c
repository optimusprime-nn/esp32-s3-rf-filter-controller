/* HTTPS management API v1. docs/API.md is the specification; keep the two in step.
 * Access follows esp32p4_nts_time_server: an unclaimed board serves only enrollment;
 * a claimed board requires a client certificate signed by the claimed CA (mTLS). */
#include "esp_https_server.h"
#include "esp_http_server.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "cJSON.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"
#include "sdkconfig.h"
#include "bootstrap_tls.h"
#include "console_line.h"
#include "der_util.h"
#include "fan.h"
#include "filter.h"
#include "management_settings.h"
#include "ownership.h"
#include "provisioning.h"
#include "temp_sensor.h"
#include "gpio_info.h"
#include "time_service.h"
#include "weather.h"
#include "sdr_power.h"
#include "ota.h"
#include "acme.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
static const char *TAG="web_tls";
static httpd_handle_t server;
static char *server_cert,*server_key,*client_ca;
static size_t server_key_len;
static bool bootstrap_cert;
static esp_timer_handle_t reboot_timer;
static cJSON *sdr_json(void);
static cJSON *ota_json(void);
static cJSON *acme_json(void);
esp_err_t web_start(void);
static unsigned claim_failures;
#define SMALL_BODY 512
#define TLS_BODY 40000
#define MAX_CLAIM_FAILURES 5
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char weather_html_start[] asm("_binary_weather_html_start");
extern const char gpio_html_start[] asm("_binary_gpio_html_start");
extern esp_err_t rf_network_get_ip(char *out, size_t len);
extern esp_err_t rf_network_get_info(char *ip, char *netmask, char *gateway, size_t len);
static int64_t now_ms(void) {
    return esp_timer_get_time()/1000;
}
static void no_store(httpd_req_t *req) {
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
}
static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg) {
    httpd_resp_set_status(req,status);
    httpd_resp_set_type(req,"text/plain; charset=utf-8");
    no_store(req);
    return httpd_resp_sendstr(req,msg);
}
/* Takes ownership of root. */
static esp_err_t send_json(httpd_req_t *req, const char *status, cJSON *root) {
    char *s=root?cJSON_PrintUnformatted(root):NULL;
    cJSON_Delete(root);
    if(!s) return send_error(req,"500 Internal Server Error","Out of memory");
    httpd_resp_set_status(req,status);
    httpd_resp_set_type(req,"application/json");
    no_store(req);
    esp_err_t e=httpd_resp_sendstr(req,s);
    cJSON_free(s);
    return e;
}
static bool header_equals(httpd_req_t *req, const char *name, const char *value) {
    char buf[32];
    return httpd_req_get_hdr_value_str(req,name,buf,sizeof(buf))==ESP_OK && !strcmp(buf,value);
}
/* Reads the whole body into a NUL-terminated heap buffer, or sends the error and returns NULL. */
static char *read_body(httpd_req_t *req, size_t cap) {
    if(!req->content_len || req->content_len>cap) { send_error(req,"400 Bad Request","Body missing or too long"); return NULL; }
    size_t len=req->content_len,got=0;
    char *buf=malloc(len+1);
    if(!buf) { send_error(req,"500 Internal Server Error","Out of memory"); return NULL; }
    while(got<len) {
        int r=httpd_req_recv(req,buf+got,len-got);
        if(r==HTTPD_SOCK_ERR_TIMEOUT) continue;
        if(r<=0) break;
        got+=(size_t)r;
    }
    if(got!=len) { rf_secure_zero(buf,got); free(buf); send_error(req,"400 Bad Request","Body incomplete"); return NULL; }
    buf[len]='\0';
    return buf;
}
/* Returns the parsed body, or NULL after sending the error response. */
static cJSON *read_json(httpd_req_t *req, size_t cap) {
    char type[64];
    if(httpd_req_get_hdr_value_str(req,"Content-Type",type,sizeof(type))!=ESP_OK || strncasecmp(type,"application/json",16)) {
        send_error(req,"415 Unsupported Media Type","Content-Type must be application/json");
        return NULL;
    }
    char *buf=read_body(req,cap);
    if(!buf) return NULL;
    size_t len=req->content_len;
    cJSON *j=cJSON_ParseWithLength(buf,len);
    rf_secure_zero(buf,len);
    free(buf);
    if(!j) send_error(req,"400 Bad Request","Invalid JSON");
    return j;
}
/* Zeroes every string in a parsed body before freeing it; bodies may hold passwords or keys. */
static void delete_secret_json(cJSON *j) {
    for(cJSON *c=j?j->child:NULL;c;c=c->next) if(cJSON_IsString(c) && c->valuestring) rf_secure_zero(c->valuestring,strlen(c->valuestring));
    cJSON_Delete(j);
}
static const char *json_string(const cJSON *obj, const char *key, size_t max) {
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(obj,key);
    if(!cJSON_IsString(v) || !v->valuestring || strnlen(v->valuestring,max+1)>max) return NULL;
    return v->valuestring;
}
static bool json_int(const cJSON *obj, const char *key, int lo, int hi, int *out) {
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(obj,key);
    if(!cJSON_IsNumber(v) || v->valuedouble<lo || v->valuedouble>hi || v->valuedouble!=(double)(int)v->valuedouble) return false;
    *out=(int)v->valuedouble;
    return true;
}
static void reboot_now(void *arg) {
    esp_restart();
}
static esp_err_t schedule_reboot(void) {
    const esp_timer_create_args_t args={.callback=reboot_now,.name="reboot"};
    if(!reboot_timer && esp_timer_create(&args,&reboot_timer)!=ESP_OK) return ESP_FAIL;
    esp_timer_stop(reboot_timer);
    return esp_timer_start_once(reboot_timer,1000000);
}
static const char *reset_reason(void) {
    switch(esp_reset_reason()) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep-sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_USB: return "usb";
    default: return "other";
    }
}
static cJSON *provisioning_json(void) {
    rf_provision_status_t p;
    rf_provision_get_status(&p);
    cJSON *o=cJSON_CreateObject();
    cJSON_AddBoolToObject(o,"wifi",p.wifi);
    cJSON_AddBoolToObject(o,"tls",p.tls);
    cJSON_AddBoolToObject(o,"management_claimed",rf_ownership_claimed());
    return o;
}
static cJSON *fan_json(void) {
    cJSON *o=cJSON_CreateObject();
    int32_t rpm=rf_fan_rpm_now();
    cJSON_AddNumberToObject(o,"percent",rf_fan_percent());
    if(rpm<0) cJSON_AddNullToObject(o,"rpm"); else cJSON_AddNumberToObject(o,"rpm",rpm);
    cJSON_AddNumberToObject(o,"pwm_gpio",CONFIG_RF_FAN_PWM_GPIO);
    cJSON_AddNumberToObject(o,"tach_gpio",CONFIG_RF_FAN_TACH_GPIO);
    return o;
}
static void station_id(char out[5], uint16_t id) { snprintf(out,5,"%04x",id); }
static cJSON *weather_json(void) {
    rf_weather_status_t w;
    rf_weather_status(&w);
    char id[5];
    cJSON *o=cJSON_CreateObject();
    cJSON_AddStringToObject(o,"receiver",rf_weather_receiver_name(w.receiver));
    cJSON_AddStringToObject(o,"model","VEVOR 7-in-1");
    if(w.receiver==RF_WEATHER_DISABLED) cJSON_AddNullToObject(o,"frequency_mhz");
    else cJSON_AddNumberToObject(o,"frequency_mhz",w.frequency_khz/1000.0);
    if(w.sensor_id_filter<0) cJSON_AddNullToObject(o,"station_filter");
    else { station_id(id,(uint16_t)w.sensor_id_filter); cJSON_AddStringToObject(o,"station_filter",id); }
    cJSON *p=cJSON_AddObjectToObject(o,"packets");
    cJSON_AddNumberToObject(p,"ok",w.packets_ok);
    cJSON_AddNumberToObject(p,"rejected",w.packets_rejected);
    cJSON_AddNumberToObject(p,"ignored",w.packets_ignored);
    cJSON *heard=cJSON_AddArrayToObject(o,"heard");
    for(size_t i=0;i<w.heard_count;i++) {
        cJSON *h=cJSON_CreateObject();
        station_id(id,w.heard[i].id);
        cJSON_AddStringToObject(h,"station",id);
        cJSON_AddNumberToObject(h,"rssi_dbm",w.heard[i].rssi_dbm);
        cJSON_AddNumberToObject(h,"age_s",(double)(w.heard[i].age_ms/1000));
        cJSON_AddItemToArray(heard,h);
    }
    if(!w.have_reading) { cJSON_AddNullToObject(o,"reading"); return o; }
    const rf_vevor_reading_t *r=&w.last;
    cJSON *x=cJSON_AddObjectToObject(o,"reading");
    station_id(id,r->id);
    cJSON_AddStringToObject(x,"station",id);
    cJSON_AddNumberToObject(x,"channel",r->channel);
    cJSON_AddNumberToObject(x,"age_s",(double)(w.age_ms/1000));
    cJSON_AddBoolToObject(x,"stale",w.age_ms>RF_WEATHER_STALE_S*1000LL);
    cJSON_AddNumberToObject(x,"rssi_dbm",w.rssi_dbm);
    cJSON_AddBoolToObject(x,"battery_ok",!r->battery_low);
    cJSON_AddNumberToObject(x,"temperature_c",r->temp_dc/10.0);
    cJSON_AddNumberToObject(x,"humidity_percent",r->humidity);
    cJSON_AddNumberToObject(x,"wind_avg_kmh",round(rf_vevor_wind_kmh(r->wind_raw)*10)/10);
    cJSON_AddNumberToObject(x,"wind_gust_kmh",round(rf_vevor_gust_kmh(r->gust_raw)*10)/10);
    cJSON_AddNumberToObject(x,"wind_dir_deg",r->wind_dir_deg);
    cJSON_AddNumberToObject(x,"rain_mm",round(rf_vevor_rain_mm(r->rain_raw)*10)/10);
    cJSON_AddNumberToObject(x,"uv_index",r->uv_index);
    cJSON_AddNumberToObject(x,"light_lux",r->light_lux);
    return o;
}
static cJSON *temperature_json(void) {
    rf_temp_reading_t r[RF_TEMP_MAX_SENSORS];
    size_t n=rf_temp_snapshot(r,RF_TEMP_MAX_SENSORS);
    cJSON *o=cJSON_CreateObject(),*list=cJSON_AddArrayToObject(o,"sensors");
    for(size_t i=0;i<n;i++) {
        cJSON *s=cJSON_CreateObject();
        cJSON_AddStringToObject(s,"id",r[i].id);
        cJSON_AddStringToObject(s,"source",rf_temp_source_name(r[i].source));
        if(r[i].source==RF_TEMP_INTERNAL) { cJSON_AddNullToObject(s,"bus"); cJSON_AddNullToObject(s,"address"); }
        else { cJSON_AddNumberToObject(s,"bus",r[i].bus); cJSON_AddNumberToObject(s,"address",r[i].address); }
        cJSON_AddBoolToObject(s,"ok",r[i].ok);
        if(r[i].ok) cJSON_AddNumberToObject(s,"celsius",r[i].millicelsius/1000.0); else cJSON_AddNullToObject(s,"celsius");
        if(r[i].ok && r[i].has_humidity) cJSON_AddNumberToObject(s,"humidity_percent",r[i].millipercent_rh/1000.0);
        else cJSON_AddNullToObject(s,"humidity_percent");
        cJSON_AddItemToArray(list,s);
    }
    return o;
}
static cJSON *config_json(const rf_management_settings_t *s) {
    cJSON *o=cJSON_CreateObject();
    cJSON_AddStringToObject(o,"hostname",s->hostname);
    cJSON_AddStringToObject(o,"fqdn",s->fqdn);
    cJSON *ntp=cJSON_AddArrayToObject(o,"ntp_servers");
    for(int i=0;i<RF_NTP_COUNT;i++) cJSON_AddItemToArray(ntp,cJSON_CreateString(s->ntp_servers[i]));
    cJSON *net=cJSON_AddObjectToObject(o,"network");
    cJSON_AddBoolToObject(net,"dhcp",s->dhcp);
    cJSON_AddStringToObject(net,"address",s->ipv4);
    cJSON_AddStringToObject(net,"netmask",s->netmask);
    cJSON_AddStringToObject(net,"gateway",s->gateway);
    cJSON_AddStringToObject(net,"dns",s->dns);
    cJSON *acme=cJSON_AddObjectToObject(o,"acme");
    cJSON_AddBoolToObject(acme,"enabled",s->acme_enabled);
    cJSON_AddStringToObject(acme,"cloudflare_zone",s->cloudflare_zone);
    cJSON_AddItemToObject(o,"provisioning",provisioning_json());
    return o;
}
/* ---- Enrollment: the only handlers on an unclaimed board ---- */
static esp_err_t enrollment_index(httpd_req_t *req) {
    static const char html[]="<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>RF Controller enrollment</title></head><body>"
        "<h1>Management enrollment required</h1>"
        "<p>This controller has not been claimed, so normal management is disabled.</p>"
        "<p>Claim it with the setup tool, or with <code>PUT /api/v1/provisioning/claim</code>. "
        "The one-time enrollment code is shown only on the serial and USB consoles.</p>"
        "<p>After the claim the controller restarts and from then on requires a client certificate signed by your management CA.</p>"
        "</body></html>";
    httpd_resp_set_type(req,"text/html; charset=utf-8");
    no_store(req);
    return httpd_resp_sendstr(req,html);
}
static esp_err_t claim_put(httpd_req_t *req) {
    if(rf_ownership_claimed()) return send_error(req,"404 Not Found","Already claimed");
    if(claim_failures>=MAX_CLAIM_FAILURES) return send_error(req,"429 Too Many Requests","Enrollment locked until reboot");
    char code[RF_OWNERSHIP_CODE_LEN+2];
    if(httpd_req_get_hdr_value_str(req,"X-Provisioning-Code",code,sizeof(code))!=ESP_OK)
        return send_error(req,"401 Unauthorized","X-Provisioning-Code header required");
    if(!rf_ownership_code_matches(code)) {
        rf_secure_zero(code,sizeof(code));
        if(++claim_failures>=MAX_CLAIM_FAILURES) {
            ESP_LOGW(TAG,"Too many invalid enrollment attempts; reboot required for a new code");
            return send_error(req,"429 Too Many Requests","Enrollment locked until reboot");
        }
        return send_error(req,"403 Forbidden","Invalid provisioning code");
    }
    char *pem=read_body(req,RF_OWNERSHIP_CA_MAX);
    if(!pem) { rf_secure_zero(code,sizeof(code)); return ESP_OK; }
    esp_err_t e=rf_ownership_claim(code,pem,req->content_len);
    free(pem);
    rf_secure_zero(code,sizeof(code));
    if(e!=ESP_OK) return send_error(req,"400 Bad Request","Invalid management CA: it must be a PEM certificate allowed to sign certificates");
    cJSON *o=cJSON_CreateObject();
    cJSON_AddBoolToObject(o,"claimed",true);
    cJSON_AddBoolToObject(o,"reboot_required",true);
    cJSON_AddStringToObject(o,"management_mode_after_reboot","mtls_authenticated");
    e=send_json(req,"202 Accepted",o);
    if(schedule_reboot()!=ESP_OK) ESP_LOGE(TAG,"Claimed, but the automatic reboot could not be scheduled; reset the board");
    return e;
}
/* ---- Management: registered only on a claimed board, behind mTLS ---- */
static esp_err_t index_handler(httpd_req_t *req) {
    httpd_resp_set_type(req,"text/html; charset=utf-8");
    no_store(req);
    return httpd_resp_sendstr(req,index_html_start);
}
static esp_err_t health_handler(httpd_req_t *req) {
    char ip[16];
    rf_management_settings_t s;
    bool ready=rf_network_get_ip(ip,sizeof(ip))==ESP_OK;
    cJSON *o=cJSON_CreateObject();
    cJSON_AddStringToObject(o,"status",ready?"ok":"not-ready");
    cJSON_AddStringToObject(o,"hostname",rf_settings_load(&s)==ESP_OK?s.hostname:"");
    cJSON_AddBoolToObject(o,"network_ready",ready);
    return send_json(req,ready?"200 OK":"503 Service Unavailable",o);
}
static esp_err_t status_handler(httpd_req_t *req) {
    rf_management_settings_t s;
    if(rf_settings_load(&s)!=ESP_OK) return send_error(req,"500 Internal Server Error","Configuration unavailable");
    const esp_app_desc_t *app=esp_app_get_description();
    char ip[16],mac_text[18];
    uint8_t mac[6]={0};
    esp_read_mac(mac,ESP_MAC_WIFI_STA);
    snprintf(mac_text,sizeof(mac_text),"%02x:%02x:%02x:%02x:%02x:%02x",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    cJSON *o=cJSON_CreateObject();
    cJSON *dev=cJSON_AddObjectToObject(o,"device");
    cJSON_AddStringToObject(dev,"hostname",s.hostname);
    cJSON_AddStringToObject(dev,"fqdn",s.fqdn);
    char mask[16],gw[16];
    if(rf_network_get_info(ip,mask,gw,sizeof(ip))==ESP_OK) {
        cJSON_AddStringToObject(dev,"ipv4",ip);
        cJSON_AddStringToObject(dev,"netmask",mask);
        cJSON_AddStringToObject(dev,"gateway",gw);
    } else cJSON_AddNullToObject(dev,"ipv4");
    cJSON_AddStringToObject(dev,"mac",mac_text);
    cJSON_AddStringToObject(dev,"firmware",app->version);
    cJSON_AddStringToObject(dev,"idf",app->idf_ver);
    cJSON *sys=cJSON_AddObjectToObject(o,"system");
    cJSON_AddNumberToObject(sys,"uptime_ms",(double)now_ms());
    cJSON_AddStringToObject(sys,"reset_reason",reset_reason());
    cJSON_AddNumberToObject(sys,"heap_internal_free",heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(sys,"heap_internal_min_free",heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(sys,"heap_psram_free",heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON *t=cJSON_AddObjectToObject(o,"time");
    cJSON_AddBoolToObject(t,"synchronized",rf_time_service_is_synchronized());
    cJSON_AddNumberToObject(t,"unix_time",(double)time(NULL));
    cJSON *ntp=cJSON_AddArrayToObject(t,"ntp_servers");
    for(int i=0;i<RF_NTP_COUNT;i++) cJSON_AddItemToArray(ntp,cJSON_CreateString(s.ntp_servers[i]));
    cJSON *rf=cJSON_AddObjectToObject(o,"rf");
    cJSON_AddNumberToObject(rf,"filter",rf_filter_current());
    cJSON_AddItemToObject(o,"fan",fan_json());
    cJSON_AddItemToObject(o,"temperature",temperature_json());
    cJSON_AddItemToObject(o,"weather",weather_json());
    cJSON_AddItemToObject(o,"sdr",sdr_json());
    cJSON_AddItemToObject(o,"ota",ota_json());
    cJSON_AddItemToObject(o,"acme",acme_json());
    cJSON_AddItemToObject(o,"provisioning",provisioning_json());
    cJSON *console=cJSON_AddObjectToObject(o,"console");
    cJSON_AddStringToObject(console,"mode","mtls");
    cJSON_AddStringToObject(console,"server_certificate",bootstrap_cert?"bootstrap":"provisioned");
    return send_json(req,"200 OK",o);
}
static esp_err_t filter_get(httpd_req_t *req) {
    cJSON *o=cJSON_CreateObject();
    cJSON_AddNumberToObject(o,"filter",rf_filter_current());
    return send_json(req,"200 OK",o);
}
static esp_err_t filter_put(httpd_req_t *req) {
    cJSON *body=read_json(req,SMALL_BODY);
    if(!body) return ESP_OK;
    int port;
    bool ok=json_int(body,"filter",1,4,&port);
    cJSON_Delete(body);
    if(!ok) return send_error(req,"400 Bad Request","filter must be 1 to 4");
    if(rf_filter_select((uint8_t)port)!=ESP_OK) return send_error(req,"500 Internal Server Error","Filter switch failed");
    return filter_get(req);
}
static esp_err_t fan_get(httpd_req_t *req) {
    return send_json(req,"200 OK",fan_json());
}
static esp_err_t fan_put(httpd_req_t *req) {
    cJSON *body=read_json(req,SMALL_BODY);
    if(!body) return ESP_OK;
    int percent;
    bool ok=json_int(body,"percent",0,100,&percent);
    cJSON_Delete(body);
    if(!ok) return send_error(req,"400 Bad Request","percent must be a whole number from 0 to 100");
    if(rf_fan_set((uint8_t)percent)!=ESP_OK) return send_error(req,"500 Internal Server Error","Fan update failed");
    return send_json(req,"200 OK",fan_json());
}
static esp_err_t temperature_get(httpd_req_t *req) {
    return send_json(req,"200 OK",temperature_json());
}
static esp_err_t weather_get(httpd_req_t *req) {
    return send_json(req,"200 OK",weather_json());
}
/* Streams the history as JSON in pieces, so 48 hours of samples never sit in RAM as cJSON. */
static esp_err_t weather_history_get(httpd_req_t *req) {
    unsigned hours=24;
    char q[32],v[8];
    if(httpd_req_get_url_query_str(req,q,sizeof(q))==ESP_OK && httpd_query_key_value(q,"hours",v,sizeof(v))==ESP_OK) {
        char *end;
        long h=strtol(v,&end,10);
        if(*end || h<1 || h>48) return send_error(req,"400 Bad Request","hours must be 1 to 48");
        hours=(unsigned)h;
    }
    uint32_t oldest,end;
    size_t cap;
    rf_weather_history_span(&oldest,&end,&cap);
    uint32_t now_s=(uint32_t)(now_ms()/1000);
    httpd_resp_set_type(req,"application/json");
    no_store(req);
    char buf[1536];
    int n=snprintf(buf,sizeof(buf),"{\"interval_s\":%d,\"capacity_hours\":%u,\"fields\":[\"age_s\",\"temperature_c\",\"humidity_percent\","
        "\"wind_avg_kmh\",\"wind_gust_kmh\",\"wind_dir_deg\",\"rain_mm\",\"uv_index\",\"light_lux\",\"rssi_dbm\"],\"points\":[",
        RF_WEATHER_HISTORY_INTERVAL_S,(unsigned)(cap/60));
    if(httpd_resp_send_chunk(req,buf,n)!=ESP_OK) return ESP_FAIL;
    rf_weather_sample_t s[16];
    uint32_t seq=oldest;
    bool first=true;
    for(;;) {
        size_t got=rf_weather_history_copy(seq,s,16,&seq);
        if(!got) break;
        size_t len=0;
        for(size_t i=0;i<got;i++) {
            uint32_t age=now_s-s[i].t_s;
            if(age>hours*3600u) continue;
            len+=(size_t)snprintf(buf+len,sizeof(buf)-len,"%s[%lu,%.1f,%u,%.1f,%.1f,%u,%.1f,%d,%lu,%d]",first?"":",",
                (unsigned long)age,s[i].temp_dc/10.0,(unsigned)s[i].humidity,rf_vevor_wind_kmh(s[i].wind_raw),
                rf_vevor_gust_kmh(s[i].gust_raw),(unsigned)s[i].wind_dir_deg,rf_vevor_rain_mm(s[i].rain_raw),
                (int)s[i].uv_index,(unsigned long)s[i].light_lux,(int)s[i].rssi_dbm);
            first=false;
        }
        if(len && httpd_resp_send_chunk(req,buf,(ssize_t)len)!=ESP_OK) return ESP_FAIL;
    }
    if(httpd_resp_send_chunk(req,"]}",2)!=ESP_OK) return ESP_FAIL;
    return httpd_resp_send_chunk(req,NULL,0);
}
static esp_err_t weather_page(httpd_req_t *req) {
    httpd_resp_set_type(req,"text/html; charset=utf-8");
    no_store(req);
    return httpd_resp_sendstr(req,weather_html_start);
}
static esp_err_t gpio_page(httpd_req_t *req) {
    httpd_resp_set_type(req,"text/html; charset=utf-8");
    no_store(req);
    return httpd_resp_sendstr(req,gpio_html_start);
}
static void add_str_or_null(cJSON *o, const char *k, const char *v) {
    if(v && v[0]) cJSON_AddStringToObject(o,k,v); else cJSON_AddNullToObject(o,k);
}
static esp_err_t gpio_get(httpd_req_t *req) {
    rf_gpio_info_t *pins=calloc(RF_GPIO_INFO_MAX,sizeof(*pins));
    if(!pins) return send_error(req,"500 Internal Server Error","Out of memory");
    size_t n=rf_gpio_info(pins,RF_GPIO_INFO_MAX);
    cJSON *o=cJSON_CreateObject(),*list=cJSON_AddArrayToObject(o,"pins");
    for(size_t i=0;i<n;i++) {
        const rf_gpio_info_t *p=&pins[i];
        cJSON *x=cJSON_CreateObject();
        cJSON_AddNumberToObject(x,"gpio",p->gpio);
        add_str_or_null(x,"function",p->function);
        add_str_or_null(x,"group",p->group);
        add_str_or_null(x,"note",p->note);
        add_str_or_null(x,"detail",p->detail);
        cJSON_AddBoolToObject(x,"config_read",p->config_read);
        if(p->level<0) cJSON_AddNullToObject(x,"level"); else cJSON_AddNumberToObject(x,"level",p->level);
        cJSON_AddBoolToObject(x,"input",p->input);
        cJSON_AddBoolToObject(x,"output",p->output);
        cJSON_AddBoolToObject(x,"open_drain",p->open_drain);
        cJSON_AddBoolToObject(x,"pull_up",p->pull_up);
        cJSON_AddBoolToObject(x,"pull_down",p->pull_down);
        cJSON_AddBoolToObject(x,"peripheral",p->peripheral);
        cJSON_AddNumberToObject(x,"drive",p->drive);
        cJSON_AddItemToArray(list,x);
    }
    free(pins);
    return send_json(req,"200 OK",o);
}
static esp_err_t weather_station_put(httpd_req_t *req) {
    cJSON *body=read_json(req,SMALL_BODY);
    if(!body) return ESP_OK;
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(body,"station");
    long id=-2;
    if(cJSON_IsNull(v)) id=-1;
    else if(cJSON_IsString(v) && strlen(v->valuestring)==4) {
        char *end;
        id=strtol(v->valuestring,&end,16);
        if(*end) id=-2;
    }
    cJSON_Delete(body);
    if(id<-1) return send_error(req,"400 Bad Request","station must be a 4-digit hex ID or null");
    esp_err_t e=rf_weather_set_station((int32_t)id);
    if(e==ESP_ERR_NOT_SUPPORTED || e==ESP_ERR_INVALID_STATE) return send_error(req,"409 Conflict","Weather receiver is not running");
    if(e!=ESP_OK) return send_error(req,"500 Internal Server Error","Could not save the station");
    return send_json(req,"200 OK",weather_json());
}
static esp_err_t config_get(httpd_req_t *req) {
    rf_management_settings_t s;
    if(rf_settings_load(&s)!=ESP_OK) return send_error(req,"500 Internal Server Error","Configuration unavailable");
    return send_json(req,"200 OK",config_json(&s));
}
static bool copy_field(char *dst, size_t cap, const cJSON *obj, const char *key) {
    const char *v=json_string(obj,key,cap-1);
    if(!v) return false;
    strcpy(dst,v);
    return true;
}
typedef bool (*settings_edit_t)(rf_management_settings_t *s, const cJSON *body);
/* Shared by the hostname, NTP and network calls: edit a copy, validate it whole, then save. */
static esp_err_t settings_put(httpd_req_t *req, settings_edit_t edit) {
    rf_management_settings_t s;
    if(rf_settings_load(&s)!=ESP_OK) return send_error(req,"500 Internal Server Error","Configuration unavailable");
    cJSON *body=read_json(req,SMALL_BODY);
    if(!body) return ESP_OK;
    bool ok=edit(&s,body) && rf_settings_valid(&s);
    cJSON_Delete(body);
    if(!ok) return send_error(req,"400 Bad Request","Invalid or inconsistent settings");
    if(rf_settings_save(&s)!=ESP_OK) return send_error(req,"500 Internal Server Error","Could not save settings");
    return send_json(req,"200 OK",config_json(&s));
}
/* {"hostname":"rf1"} and/or {"fqdn":"rf1.example.com"}, the name the certificate is issued for. */
static bool edit_hostname(rf_management_settings_t *s, const cJSON *b) {
    bool h=cJSON_GetObjectItemCaseSensitive(b,"hostname")!=NULL,f=cJSON_GetObjectItemCaseSensitive(b,"fqdn")!=NULL;
    if(!h && !f) return false;
    if(h && !copy_field(s->hostname,sizeof(s->hostname),b,"hostname")) return false;
    if(f && !copy_field(s->fqdn,sizeof(s->fqdn),b,"fqdn")) return false;
    return true;
}
/* {"servers":["primary","secondary"]}: exactly two names. */
static bool edit_ntp(rf_management_settings_t *s, const cJSON *b) {
    const cJSON *list=cJSON_GetObjectItemCaseSensitive(b,"servers");
    if(!cJSON_IsArray(list) || cJSON_GetArraySize(list)!=RF_NTP_COUNT) return false;
    for(int i=0;i<RF_NTP_COUNT;i++) {
        const cJSON *v=cJSON_GetArrayItem(list,i);
        if(!cJSON_IsString(v) || strlen(v->valuestring)>RF_NTP_MAX) return false;
        strcpy(s->ntp_servers[i],v->valuestring);
    }
    return true;
}
static bool edit_network(rf_management_settings_t *s, const cJSON *b) {
    const cJSON *dhcp=cJSON_GetObjectItemCaseSensitive(b,"dhcp");
    if(!cJSON_IsBool(dhcp)) return false;
    s->dhcp=cJSON_IsTrue(dhcp);
    if(s->dhcp) return true;
    return copy_field(s->ipv4,sizeof(s->ipv4),b,"address") && copy_field(s->netmask,sizeof(s->netmask),b,"netmask")
        && copy_field(s->gateway,sizeof(s->gateway),b,"gateway") && copy_field(s->dns,sizeof(s->dns),b,"dns");
}
static esp_err_t hostname_put(httpd_req_t *req) { return settings_put(req,edit_hostname); }
static esp_err_t ntp_put(httpd_req_t *req) { return settings_put(req,edit_ntp); }
static esp_err_t network_put(httpd_req_t *req) { return settings_put(req,edit_network); }
static esp_err_t stored_reply(httpd_req_t *req, esp_err_t e) {
    if(e==ESP_ERR_INVALID_ARG) return send_error(req,"400 Bad Request","Value rejected");
    if(e!=ESP_OK) return send_error(req,"500 Internal Server Error",esp_err_to_name(e));
    cJSON *o=cJSON_CreateObject();
    cJSON_AddBoolToObject(o,"stored",true);
    cJSON_AddBoolToObject(o,"reboot_required",true);
    return send_json(req,"200 OK",o);
}
static esp_err_t wifi_put(httpd_req_t *req) {
    cJSON *body=read_json(req,SMALL_BODY);
    if(!body) return ESP_OK;
    const char *ssid=json_string(body,"ssid",RF_SSID_MAX),*password=json_string(body,"password",RF_WIFI_PASSWORD_MAX);
    esp_err_t e=ssid && password?rf_provision_set_wifi(ssid,password):ESP_ERR_INVALID_ARG;
    delete_secret_json(body);
    return stored_reply(req,e);
}
static esp_err_t tls_put(httpd_req_t *req) {
    cJSON *body=read_json(req,TLS_BODY);
    if(!body) return ESP_OK;
    const char *cert=json_string(body,"certificate",RF_PEM_MAX),*key=json_string(body,"private_key",RF_PEM_MAX);
    esp_err_t e=cert && key?rf_provision_set_tls(cert,key):ESP_ERR_INVALID_ARG;
    delete_secret_json(body);
    if(e==ESP_OK) rf_acme_forget_certificate();
    return stored_reply(req,e);
}
static esp_err_t tls_delete(httpd_req_t *req) {
    esp_err_t e=rf_provision_clear_tls();
    if(e==ESP_OK) rf_acme_forget_certificate();
    if(e!=ESP_OK) return send_error(req,"500 Internal Server Error",esp_err_to_name(e));
    cJSON *o=cJSON_CreateObject();
    cJSON_AddBoolToObject(o,"removed",true);
    cJSON_AddBoolToObject(o,"reboot_required",true);
    return send_json(req,"200 OK",o);
}
static esp_err_t reboot_post(httpd_req_t *req) {
    if(!header_equals(req,"X-Confirm-Reboot","REBOOT")) return send_error(req,"412 Precondition Failed","Send X-Confirm-Reboot: REBOOT");
    if(schedule_reboot()!=ESP_OK) return send_error(req,"500 Internal Server Error","Could not schedule reboot");
    cJSON *o=cJSON_CreateObject();
    cJSON_AddBoolToObject(o,"scheduled",true);
    cJSON_AddStringToObject(o,"action","reboot");
    return send_json(req,"202 Accepted",o);
}
static cJSON *sdr_json(void) {
    rf_sdr_status_t s;
    rf_sdr_status(&s);
    cJSON *o=cJSON_CreateObject();
    cJSON_AddStringToObject(o,"state",rf_sdr_state_name(s.state));
    cJSON_AddBoolToObject(o,"powered",s.state!=RF_SDR_POWER_OFF);
    cJSON_AddNumberToObject(o,"soft_resets",s.soft_resets);
    cJSON_AddNumberToObject(o,"hard_resets",s.hard_resets);
    if(s.last_action_ms<0) { cJSON_AddNullToObject(o,"last_action"); cJSON_AddNullToObject(o,"last_action_age_ms"); }
    else {
        cJSON_AddStringToObject(o,"last_action",s.last_was_hard?"power_cycle":"reboot");
        cJSON_AddNumberToObject(o,"last_action_age_ms",(double)(now_ms()-s.last_action_ms));
    }
    cJSON_AddNumberToObject(o,"soft_reset_gpio",CONFIG_RF_SDR_SOFT_RESET_GPIO);
    cJSON_AddNumberToObject(o,"relay_gpio",CONFIG_RF_SDR_RELAY_GPIO);
    return o;
}
static esp_err_t sdr_get(httpd_req_t *req) { return send_json(req,"200 OK",sdr_json()); }
static esp_err_t sdr_action(httpd_req_t *req, const char *header, const char *word, bool hard) {
    if(!header_equals(req,header,word)) {
        char m[64];
        snprintf(m,sizeof(m),"Send %s: %s",header,word);
        return send_error(req,"412 Precondition Failed",m);
    }
    esp_err_t e=hard?rf_sdr_hard_reset():rf_sdr_soft_reset();
    if(e==ESP_ERR_INVALID_STATE) return send_error(req,"409 Conflict","An SDR reset is in progress or just finished; try again shortly");
    if(e!=ESP_OK) return send_error(req,"500 Internal Server Error",esp_err_to_name(e));
    cJSON *o=sdr_json();
    cJSON_AddStringToObject(o,"action",hard?"power_cycle":"reboot");
    return send_json(req,"202 Accepted",o);
}
static esp_err_t sdr_reboot_post(httpd_req_t *req) { return sdr_action(req,"X-Confirm-SDR-Reboot","REBOOT",false); }
static esp_err_t sdr_power_cycle_post(httpd_req_t *req) { return sdr_action(req,"X-Confirm-SDR-Power-Cycle","POWER-CYCLE",true); }
/* ---- Firmware updates (as on esp32p4_nts_time_server) ---- */
static cJSON *acme_json(void) {
    rf_acme_status_t *a=calloc(1,sizeof(*a));
    cJSON *o=cJSON_CreateObject();
    if(!a) return o;
    rf_acme_status(a);
    rf_management_settings_t s;
    bool have=rf_settings_load(&s)==ESP_OK;
    cJSON_AddStringToObject(o,"fqdn",have?s.fqdn:"");
    cJSON_AddStringToObject(o,"cloudflare_zone",have?s.cloudflare_zone:"");
    cJSON_AddBoolToObject(o,"cloudflare_token_present",a->token_present);
    cJSON_AddBoolToObject(o,"automatic_renewal",have && s.acme_enabled);
    cJSON_AddBoolToObject(o,"clock_synchronized",rf_time_service_is_synchronized());
    cJSON *r=cJSON_AddObjectToObject(o,"request");
    cJSON_AddStringToObject(r,"state",rf_acme_state_name(a->state));
    cJSON_AddStringToObject(r,"environment",a->production?"production":"staging");
    cJSON_AddStringToObject(r,"stage",a->stage);
    if(a->error[0]) cJSON_AddStringToObject(r,"error",a->error); else cJSON_AddNullToObject(r,"error");
    cJSON_AddNumberToObject(r,"started",(double)a->started);
    cJSON_AddNumberToObject(r,"finished",(double)a->finished);
    cJSON *c=cJSON_AddObjectToObject(o,"certificate");
    cJSON_AddBoolToObject(c,"present",a->cert_present);
    if(a->cert_present) {
        cJSON_AddStringToObject(c,"source",!a->cert_from_acme?"manual":a->cert_production?"letsencrypt":"letsencrypt-staging");
        cJSON_AddStringToObject(c,"hostname",a->cert_hostname);
        cJSON_AddBoolToObject(c,"hostname_matches",have && a->cert_from_acme && !strcmp(a->cert_hostname,s.fqdn));
        cJSON_AddStringToObject(c,"issuer",a->cert_issuer);
        cJSON_AddStringToObject(c,"valid_from",a->cert_valid_from);
        cJSON_AddStringToObject(c,"valid_to",a->cert_valid_to);
        if(a->cert_days_left>=0) cJSON_AddNumberToObject(c,"days_left",a->cert_days_left); else cJSON_AddNullToObject(c,"days_left");
        cJSON_AddStringToObject(c,"sha256",a->cert_sha256);
    }
    cJSON_AddBoolToObject(c,"serving",!bootstrap_cert);
    free(a);
    return o;
}
static esp_err_t acme_get(httpd_req_t *req) { return send_json(req,"200 OK",acme_json()); }
/* {"production":true} for a trusted certificate, false for Let's Encrypt staging (untrusted, for tests). */
static esp_err_t acme_issue_post(httpd_req_t *req) {
    bool production=true;
    if(req->content_len) {
        cJSON *body=read_json(req,SMALL_BODY);
        if(!body) return ESP_OK;
        const cJSON *p=cJSON_GetObjectItemCaseSensitive(body,"production");
        bool ok=!p || cJSON_IsBool(p);
        if(cJSON_IsBool(p)) production=cJSON_IsTrue(p);
        cJSON_Delete(body);
        if(!ok) return send_error(req,"400 Bad Request","production must be true or false");
    }
    esp_err_t e=rf_acme_issue(production);
    if(e==ESP_ERR_TIMEOUT) return send_error(req,"503 Service Unavailable","The clock isn't synchronized yet; check the NTP servers");
    if(e==ESP_ERR_INVALID_ARG) return send_error(req,"409 Conflict","The certificate name isn't inside the Cloudflare zone");
    if(e==ESP_ERR_INVALID_STATE) {
        rf_acme_status_t *a=calloc(1,sizeof(*a));
        bool running=false;
        if(a) { rf_acme_status(a); running=a->state==RF_ACME_RUNNING; free(a); }
        return send_error(req,"409 Conflict",running?"A certificate request is already running":
            "Set the certificate name (fqdn) and the Cloudflare zone and token first");
    }
    if(e!=ESP_OK) return send_error(req,"500 Internal Server Error",esp_err_to_name(e));
    return send_json(req,"202 Accepted",acme_json());
}
/* {"api_token":"...","zone_name":"example.com"}: checked with Cloudflare from this board before it's stored. */
static esp_err_t cloudflare_put(httpd_req_t *req) {
    cJSON *body=read_json(req,SMALL_BODY);
    if(!body) return ESP_OK;
    const char *token=json_string(body,"api_token",256),*zone=json_string(body,"zone_name",RF_FQDN_MAX);
    int http=0;
    esp_err_t e=token && zone?rf_acme_set_cloudflare(token,zone,&http):ESP_ERR_INVALID_ARG;
    delete_secret_json(body);
    if(e==ESP_ERR_INVALID_ARG) return send_error(req,"400 Bad Request","api_token and a valid zone_name are required");
    if(e!=ESP_OK) {
        char msg[120];
        snprintf(msg,sizeof(msg),"Cloudflare didn't accept the token for that zone (%s, HTTP %d)",esp_err_to_name(e),http);
        return send_error(req,"502 Bad Gateway",msg);
    }
    return send_json(req,"200 OK",acme_json());
}
static esp_err_t cloudflare_delete(httpd_req_t *req) {
    esp_err_t e=rf_acme_clear_cloudflare();
    if(e!=ESP_OK) return send_error(req,"500 Internal Server Error",esp_err_to_name(e));
    return send_json(req,"200 OK",acme_json());
}
static cJSON *ota_json(void) {
    rf_ota_status_t s;
    rf_ota_status(&s);
    char rb[33];
    bool rollback=!s.update_in_progress && rf_ota_rollback_target(rb,sizeof(rb))==ESP_OK;
    cJSON *o=cJSON_CreateObject();
    cJSON_AddBoolToObject(o,"rollback_available",rollback);
    cJSON_AddStringToObject(o,"rollback_version",rollback?rb:"");
    cJSON_AddBoolToObject(o,"update_in_progress",s.update_in_progress);
    cJSON_AddBoolToObject(o,"pending_verify",s.pending_verify);
    cJSON_AddNumberToObject(o,"attempts",s.attempts);
    cJSON_AddNumberToObject(o,"successes",s.successes);
    cJSON_AddNumberToObject(o,"failures",s.failures);
    cJSON_AddNumberToObject(o,"confirmations",s.confirmations);
    cJSON_AddNumberToObject(o,"rollbacks_detected",s.rollbacks_detected);
    cJSON_AddNumberToObject(o,"bytes_written",(double)s.bytes_written);
    cJSON_AddStringToObject(o,"running_partition",s.running_partition);
    cJSON_AddStringToObject(o,"boot_partition",s.boot_partition);
    cJSON_AddStringToObject(o,"next_partition",s.next_partition);
    cJSON_AddStringToObject(o,"running_version",s.running_version);
    cJSON_AddStringToObject(o,"previous_version",s.previous_version);
    cJSON_AddStringToObject(o,"running_project",s.running_project);
    cJSON_AddStringToObject(o,"last_result",s.last_result);
    return o;
}
static esp_err_t ota_status_get(httpd_req_t *req) { return send_json(req,"200 OK",ota_json()); }
/* Body: the raw application image (build/rf_filter_controller.bin). */
static esp_err_t ota_image_post(httpd_req_t *req) {
    if(req->content_len<=0) return send_error(req,"400 Bad Request","Firmware image required");
    const size_t chunk=4096;
    uint8_t *buf=malloc(chunk);
    if(!buf) return send_error(req,"500 Internal Server Error","Out of memory");
    size_t remaining=req->content_len,have=0;
    int timeouts=0;
    bool begun=false,first=true;
    const char *fail=NULL,*status="400 Bad Request";
    while(remaining && !fail) {
        /* The first chunk is read whole, so the image header can be checked before any flash is touched. */
        size_t want=chunk-have;
        if(want>remaining) want=remaining;
        int r=httpd_req_recv(req,(char*)buf+have,want);
        if(r==HTTPD_SOCK_ERR_TIMEOUT) { if(++timeouts>6) fail="Upload stalled"; continue; }
        if(r<=0) { fail="Upload interrupted"; break; }
        timeouts=0;
        have+=(size_t)r;
        remaining-=(size_t)r;
        if(have<chunk && remaining) continue;
        if(first) {
            first=false;
            if(rf_ota_check_header(buf,have)!=ESP_OK) { fail="Not a firmware image for this controller"; break; }
            esp_err_t e=rf_ota_begin(req->content_len);
            if(e==ESP_ERR_INVALID_STATE || e==ESP_ERR_TIMEOUT) { fail="Another update is in progress"; status="409 Conflict"; break; }
            if(e!=ESP_OK) { fail="Image too large or no update slot"; break; }
            begun=true;
        }
        if(rf_ota_write(buf,have)!=ESP_OK) { fail="Flash write failed"; status="500 Internal Server Error"; break; }
        have=0;
    }
    free(buf);
    if(fail) {
        if(begun) rf_ota_abort();
        return send_error(req,status,fail);
    }
    if(rf_ota_finish()!=ESP_OK) return send_error(req,"400 Bad Request","Image failed verification");
    cJSON *o=cJSON_CreateObject();
    cJSON_AddBoolToObject(o,"staged",true);
    cJSON_AddBoolToObject(o,"reboot_required",true);
    return send_json(req,"202 Accepted",o);
}
static esp_err_t ota_confirm_post(httpd_req_t *req) {
    if(rf_ota_confirm()!=ESP_OK) return send_error(req,"500 Internal Server Error","Could not confirm the running image");
    cJSON *o=cJSON_CreateObject();
    cJSON_AddBoolToObject(o,"confirmed",true);
    return send_json(req,"200 OK",o);
}
static esp_err_t ota_rollback_post(httpd_req_t *req) {
    if(!header_equals(req,"X-Confirm-Rollback","ROLLBACK")) return send_error(req,"412 Precondition Failed","Send X-Confirm-Rollback: ROLLBACK");
    char version[33];
    if(rf_ota_rollback_target(version,sizeof(version))!=ESP_OK) return send_error(req,"409 Conflict","No previous image available for rollback");
    esp_err_t e=rf_ota_rollback();
    if(e==ESP_ERR_NOT_FOUND || e==ESP_ERR_TIMEOUT || e==ESP_ERR_INVALID_STATE) return send_error(req,"409 Conflict","Rollback unavailable: update in progress or no previous image");
    if(e!=ESP_OK) return send_error(req,"500 Internal Server Error","Previous image failed verification");
    if(schedule_reboot()!=ESP_OK) return send_error(req,"500 Internal Server Error","Rollback staged; reboot to activate");
    cJSON *o=cJSON_CreateObject();
    cJSON_AddBoolToObject(o,"scheduled",true);
    cJSON_AddStringToObject(o,"action","rollback");
    cJSON_AddStringToObject(o,"version",version);
    return send_json(req,"202 Accepted",o);
}
static esp_err_t metrics_get(httpd_req_t *req) {
    /* Static: the server handles one request at a time, and this is too big for its stack. */
    static char buf[6144];
    size_t n=0;
    rf_temp_reading_t r[RF_TEMP_MAX_SENSORS];
    size_t count=rf_temp_snapshot(r,RF_TEMP_MAX_SENSORS);
    int32_t rpm=rf_fan_rpm_now();
#define EMIT(...) do { if(n<sizeof(buf)) n+=(size_t)snprintf(buf+n,sizeof(buf)-n,__VA_ARGS__); } while(0)
    EMIT("# TYPE rf_uptime_seconds gauge\nrf_uptime_seconds %lld\n",(long long)(now_ms()/1000));
    EMIT("# TYPE rf_filter_selected gauge\nrf_filter_selected %u\n",(unsigned)rf_filter_current());
    EMIT("# TYPE rf_fan_duty_percent gauge\nrf_fan_duty_percent %u\n",(unsigned)rf_fan_percent());
    if(rpm>=0) EMIT("# TYPE rf_fan_rpm gauge\nrf_fan_rpm %ld\n",(long)rpm);
    EMIT("# TYPE rf_temperature_celsius gauge\n");
    for(size_t i=0;i<count;i++) if(r[i].ok) EMIT("rf_temperature_celsius{sensor=\"%s\"} %.3f\n",r[i].id,r[i].millicelsius/1000.0);
    EMIT("# TYPE rf_temperature_sensor_ok gauge\n");
    for(size_t i=0;i<count;i++) EMIT("rf_temperature_sensor_ok{sensor=\"%s\"} %d\n",r[i].id,r[i].ok?1:0);
    EMIT("# TYPE rf_humidity_percent gauge\n");
    for(size_t i=0;i<count;i++) if(r[i].ok && r[i].has_humidity) EMIT("rf_humidity_percent{sensor=\"%s\"} %.2f\n",r[i].id,r[i].millipercent_rh/1000.0);
    rf_weather_status_t w;
    rf_weather_status(&w);
    if(w.receiver==RF_WEATHER_RUNNING) {
        EMIT("# TYPE rf_weather_packets_total counter\n");
        EMIT("rf_weather_packets_total{result=\"ok\"} %lu\n",(unsigned long)w.packets_ok);
        EMIT("rf_weather_packets_total{result=\"rejected\"} %lu\n",(unsigned long)w.packets_rejected);
        EMIT("rf_weather_packets_total{result=\"ignored\"} %lu\n",(unsigned long)w.packets_ignored);
    }
    if(w.have_reading) {
        const rf_vevor_reading_t *r=&w.last;
        char id[5];
        station_id(id,r->id);
        EMIT("# TYPE rf_weather_last_seen_seconds gauge\nrf_weather_last_seen_seconds{station=\"%s\"} %lld\n",id,(long long)(w.age_ms/1000));
        EMIT("# TYPE rf_weather_rssi_dbm gauge\nrf_weather_rssi_dbm{station=\"%s\"} %d\n",id,w.rssi_dbm);
        EMIT("# TYPE rf_weather_battery_ok gauge\nrf_weather_battery_ok{station=\"%s\"} %d\n",id,r->battery_low?0:1);
        EMIT("# TYPE rf_weather_temperature_celsius gauge\nrf_weather_temperature_celsius{station=\"%s\"} %.1f\n",id,r->temp_dc/10.0);
        EMIT("# TYPE rf_weather_humidity_percent gauge\nrf_weather_humidity_percent{station=\"%s\"} %u\n",id,(unsigned)r->humidity);
        EMIT("# TYPE rf_weather_wind_avg_kmh gauge\nrf_weather_wind_avg_kmh{station=\"%s\"} %.2f\n",id,rf_vevor_wind_kmh(r->wind_raw));
        EMIT("# TYPE rf_weather_wind_gust_kmh gauge\nrf_weather_wind_gust_kmh{station=\"%s\"} %.2f\n",id,rf_vevor_gust_kmh(r->gust_raw));
        EMIT("# TYPE rf_weather_wind_direction_degrees gauge\nrf_weather_wind_direction_degrees{station=\"%s\"} %u\n",id,(unsigned)r->wind_dir_deg);
        EMIT("# TYPE rf_weather_rain_mm_total counter\nrf_weather_rain_mm_total{station=\"%s\"} %.3f\n",id,rf_vevor_rain_mm(r->rain_raw));
        EMIT("# TYPE rf_weather_uv_index gauge\nrf_weather_uv_index{station=\"%s\"} %d\n",id,(int)r->uv_index);
        EMIT("# TYPE rf_weather_light_lux gauge\nrf_weather_light_lux{station=\"%s\"} %lu\n",id,(unsigned long)r->light_lux);
    }
    rf_ota_status_t ota;
    rf_ota_status(&ota);
    EMIT("# TYPE rf_ota_pending_verify gauge\nrf_ota_pending_verify %d\n",ota.pending_verify?1:0);
    EMIT("# TYPE rf_ota_rollbacks_detected_total counter\nrf_ota_rollbacks_detected_total %lu\n",(unsigned long)ota.rollbacks_detected);
    rf_acme_status_t *acme=calloc(1,sizeof(*acme));
    if(acme) {
        rf_acme_status(acme);
        if(acme->cert_present) EMIT("# TYPE rf_tls_certificate_expiry_seconds gauge\nrf_tls_certificate_expiry_seconds %lld\n",(long long)acme->cert_expires);
        EMIT("# TYPE rf_acme_request_failed gauge\nrf_acme_request_failed %d\n",acme->state==RF_ACME_FAILED?1:0);
        free(acme);
    }
    rf_sdr_status_t sdr;
    rf_sdr_status(&sdr);
    EMIT("# TYPE rf_sdr_powered gauge\nrf_sdr_powered %d\n",sdr.state==RF_SDR_POWER_OFF?0:1);
    EMIT("# TYPE rf_sdr_resets_total counter\n");
    EMIT("rf_sdr_resets_total{type=\"reboot\"} %lu\n",(unsigned long)sdr.soft_resets);
    EMIT("rf_sdr_resets_total{type=\"power_cycle\"} %lu\n",(unsigned long)sdr.hard_resets);
    EMIT("# TYPE rf_heap_internal_free_bytes gauge\nrf_heap_internal_free_bytes %u\n",(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
#undef EMIT
    if(n>=sizeof(buf)) return send_error(req,"500 Internal Server Error","Metrics too large");
    httpd_resp_set_type(req,"text/plain; version=0.0.4");
    no_store(req);
    return httpd_resp_send(req,buf,(ssize_t)n);
}
/* ---- Server credentials ---- */
static esp_err_t load_pem(nvs_handle_t nvs, const char *name, char **out, size_t *len) {
    *out=NULL; *len=0;
    esp_err_t err=nvs_get_blob(nvs,name,NULL,len);
    if(err!=ESP_OK || *len<32 || *len>16384) return ESP_ERR_INVALID_SIZE;
    char *buf=calloc(1,*len+1);
    if(!buf) return ESP_ERR_NO_MEM;
    size_t actual=*len;
    err=nvs_get_blob(nvs,name,buf,&actual);
    if(err!=ESP_OK || actual!=*len || buf[actual-1]!='\0') { rf_secure_zero(buf,*len+1); free(buf); return ESP_ERR_INVALID_STATE; }
    *out=buf;
    return ESP_OK;
}
/* Uses the certificate stored with set-tls or PUT /api/v1/config/tls, otherwise the board's bootstrap certificate. */
static esp_err_t load_server_credentials(size_t *cert_len) {
    nvs_handle_t h;
    if(nvs_open("tls",NVS_READONLY,&h)==ESP_OK) {
        esp_err_t e=load_pem(h,"cert",&server_cert,cert_len);
        if(e==ESP_OK) e=load_pem(h,"key",&server_key,&server_key_len);
        nvs_close(h);
        if(e==ESP_OK) { bootstrap_cert=false; return ESP_OK; }
        free(server_cert); server_cert=NULL;
        ESP_LOGW(TAG,"Stored TLS certificate unusable (%s); using the bootstrap certificate",esp_err_to_name(e));
    }
    bootstrap_cert=true;
    return rf_bootstrap_tls_load(&server_cert,cert_len,&server_key,&server_key_len);
}
/* SHA-256 of the certificate HTTPS serves, for the USB console's status command. */
static char served_sha256[65];
/* The setup tool reads this line from the console and pins the certificate by it. */
static void log_server_fingerprint(size_t cert_len) {
    char hex[65];
    served_sha256[0]=0;
    if(bootstrap_cert) {
        if(rf_bootstrap_tls_sha256(hex)==ESP_OK) {
            ESP_LOGW(TAG,"Bootstrap certificate SHA-256: %s",hex);
            memcpy(served_sha256,hex,sizeof(served_sha256));
        }
        return;
    }
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    uint8_t d[32];
    size_t n=0;
    if(!mbedtls_x509_crt_parse(&crt,(const unsigned char*)server_cert,cert_len) &&
       psa_hash_compute(PSA_ALG_SHA_256,crt.raw.p,crt.raw.len,d,sizeof(d),&n)==PSA_SUCCESS && n==32) {
        rf_hex_upper(d,sizeof(d),hex);
        ESP_LOGW(TAG,"Server certificate SHA-256: %s",hex);
        memcpy(served_sha256,hex,sizeof(served_sha256));
    }
    mbedtls_x509_crt_free(&crt);
}
static void free_credentials(void) {
    if(server_key) { rf_secure_zero(server_key,server_key_len); free(server_key); server_key=NULL; }
    free(server_cert); server_cert=NULL;
    free(client_ca); client_ca=NULL;
}
static volatile bool started;
/* True once HTTPS management is serving; the OTA soak checks it before confirming. */
bool web_started(void) { return started; }
/* Stops HTTPS and starts it again with the stored certificate. Not for use from a request handler. */
esp_err_t web_restart(void) {
    if(server) { httpd_ssl_stop(server); server=NULL; }
    started=false;
    free_credentials();
    return web_start();
}
bool web_certificate_sha256(char out[65], bool *bootstrap) {
    if(!started || !served_sha256[0]) return false;
    memcpy(out,served_sha256,sizeof(served_sha256));
    *bootstrap=bootstrap_cert;
    return true;
}
esp_err_t web_start(void) {
    if(server) return ESP_ERR_INVALID_STATE;
    size_t cert_len=0,ca_len=0;
    esp_err_t err=load_server_credentials(&cert_len);
    bool claimed=rf_ownership_claimed();
    if(err==ESP_OK && claimed) err=rf_ownership_load_ca(&client_ca,&ca_len);
    if(err!=ESP_OK) { free_credentials(); ESP_LOGE(TAG,"HTTPS management disabled: %s",esp_err_to_name(err)); return err; }
    const httpd_uri_t enrollment[]={
        {.uri="/",.method=HTTP_GET,.handler=enrollment_index},
        {.uri="/api/v1/provisioning/claim",.method=HTTP_PUT,.handler=claim_put},
    };
    const httpd_uri_t management[]={
        {.uri="/",.method=HTTP_GET,.handler=index_handler},
        {.uri="/api/v1/health",.method=HTTP_GET,.handler=health_handler},
        {.uri="/metrics",.method=HTTP_GET,.handler=metrics_get},
        {.uri="/api/v1/status",.method=HTTP_GET,.handler=status_handler},
        {.uri="/api/v1/rf/filter",.method=HTTP_GET,.handler=filter_get},
        {.uri="/api/v1/rf/filter",.method=HTTP_PUT,.handler=filter_put},
        {.uri="/api/v1/fan",.method=HTTP_GET,.handler=fan_get},
        {.uri="/api/v1/fan",.method=HTTP_PUT,.handler=fan_put},
        {.uri="/api/v1/sensors/temperature",.method=HTTP_GET,.handler=temperature_get},
        {.uri="/weather",.method=HTTP_GET,.handler=weather_page},
        {.uri="/gpio",.method=HTTP_GET,.handler=gpio_page},
        {.uri="/api/v1/gpio",.method=HTTP_GET,.handler=gpio_get},
        {.uri="/api/v1/sensors/weather",.method=HTTP_GET,.handler=weather_get},
        {.uri="/api/v1/sensors/weather/history",.method=HTTP_GET,.handler=weather_history_get},
        {.uri="/api/v1/sensors/weather/station",.method=HTTP_PUT,.handler=weather_station_put},
        {.uri="/api/v1/config",.method=HTTP_GET,.handler=config_get},
        {.uri="/api/v1/config/hostname",.method=HTTP_PUT,.handler=hostname_put},
        {.uri="/api/v1/config/ntp",.method=HTTP_PUT,.handler=ntp_put},
        {.uri="/api/v1/config/network",.method=HTTP_PUT,.handler=network_put},
        {.uri="/api/v1/config/wifi",.method=HTTP_PUT,.handler=wifi_put},
        {.uri="/api/v1/config/tls",.method=HTTP_PUT,.handler=tls_put},
        {.uri="/api/v1/config/tls",.method=HTTP_DELETE,.handler=tls_delete},
        {.uri="/api/v1/system/reboot",.method=HTTP_POST,.handler=reboot_post},
        {.uri="/api/v1/sdr",.method=HTTP_GET,.handler=sdr_get},
        {.uri="/api/v1/ota/status",.method=HTTP_GET,.handler=ota_status_get},
        {.uri="/api/v1/ota/image",.method=HTTP_POST,.handler=ota_image_post},
        {.uri="/api/v1/ota/confirm",.method=HTTP_POST,.handler=ota_confirm_post},
        {.uri="/api/v1/ota/rollback",.method=HTTP_POST,.handler=ota_rollback_post},
        {.uri="/api/v1/sdr/reboot",.method=HTTP_POST,.handler=sdr_reboot_post},
        {.uri="/api/v1/sdr/power-cycle",.method=HTTP_POST,.handler=sdr_power_cycle_post},
        {.uri="/api/v1/acme",.method=HTTP_GET,.handler=acme_get},
        {.uri="/api/v1/acme/issue",.method=HTTP_POST,.handler=acme_issue_post},
        {.uri="/api/v1/config/cloudflare",.method=HTTP_PUT,.handler=cloudflare_put},
        {.uri="/api/v1/config/cloudflare",.method=HTTP_DELETE,.handler=cloudflare_delete},
    };
    const httpd_uri_t *routes=claimed?management:enrollment;
    size_t count=claimed?sizeof(management)/sizeof(management[0]):sizeof(enrollment)/sizeof(enrollment[0]);
    httpd_ssl_config_t cfg=HTTPD_SSL_CONFIG_DEFAULT();
    cfg.servercert=(const unsigned char*)server_cert;
    cfg.servercert_len=cert_len;
    cfg.prvtkey_pem=(const unsigned char*)server_key;
    cfg.prvtkey_len=server_key_len;
    /* With a CA set, the TLS handshake fails for any client without a certificate it signed. */
    cfg.cacert_pem=claimed?(const unsigned char*)client_ca:NULL;
    cfg.cacert_len=claimed?ca_len:0;
    cfg.httpd.max_uri_handlers=count;
    cfg.httpd.lru_purge_enable=true;
    err=httpd_ssl_start(&server,&cfg);
    for(size_t i=0;i<count && err==ESP_OK;i++) err=httpd_register_uri_handler(server,&routes[i]);
    if(err!=ESP_OK) {
        if(server) { httpd_ssl_stop(server); server=NULL; }
        free_credentials();
        ESP_LOGE(TAG,"HTTPS management disabled: %s",esp_err_to_name(err));
        return err;
    }
    if(claimed) ESP_LOGW(TAG,"mTLS management console active on TCP/443; client certificate required");
    else ESP_LOGW(TAG,"UNPROVISIONED enrollment-only HTTPS active on TCP/443; normal management handlers are disabled");
    log_server_fingerprint(cert_len);
    started=true;
    return ESP_OK;
}
