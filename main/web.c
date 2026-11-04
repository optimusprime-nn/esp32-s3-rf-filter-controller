/* HTTPS management API v1. docs/API.md is the specification; keep the two in step. */
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
#include "sdkconfig.h"
#include "console_line.h"
#include "fan.h"
#include "filter.h"
#include "management_settings.h"
#include "provisioning.h"
#include "session.h"
#include "temp_sensor.h"
#include "time_service.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
static const char *TAG="web_tls";
static httpd_handle_t server;
static char *server_cert,*server_key;
static esp_timer_handle_t reboot_timer;
#define SESSION_COOKIE "rf_session"
#define SMALL_BODY 512
#define TLS_BODY 40000
extern const char index_html_start[] asm("_binary_index_html_start");
extern esp_err_t rf_network_get_ip(char *out, size_t len);
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
static bool logged_in(httpd_req_t *req) {
    char token[RF_SESSION_TOKEN_LEN+1];
    size_t n=sizeof(token);
    return httpd_req_get_cookie_val(req,SESSION_COOKIE,token,&n)==ESP_OK && rf_session_check(token,now_ms());
}
/* Returns the parsed body, or NULL after sending the error response. */
static cJSON *read_json(httpd_req_t *req, size_t cap) {
    char type[64];
    if(httpd_req_get_hdr_value_str(req,"Content-Type",type,sizeof(type))!=ESP_OK || strncasecmp(type,"application/json",16)) {
        send_error(req,"415 Unsupported Media Type","Content-Type must be application/json");
        return NULL;
    }
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
    cJSON *j=got==len?cJSON_ParseWithLength(buf,len):NULL;
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
    cJSON_AddBoolToObject(o,"device_password",p.device_password);
    cJSON_AddBoolToObject(o,"tls",p.tls);
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
static cJSON *temperature_json(void) {
    rf_temp_reading_t r[RF_TEMP_MAX_SENSORS];
    size_t n=rf_temp_snapshot(r,RF_TEMP_MAX_SENSORS);
    cJSON *o=cJSON_CreateObject(),*list=cJSON_AddArrayToObject(o,"sensors");
    for(size_t i=0;i<n;i++) {
        cJSON *s=cJSON_CreateObject();
        cJSON_AddStringToObject(s,"id",r[i].id);
        cJSON_AddStringToObject(s,"source",r[i].internal?"internal":"lm75");
        if(r[i].internal) cJSON_AddNullToObject(s,"address"); else cJSON_AddNumberToObject(s,"address",r[i].address);
        cJSON_AddBoolToObject(s,"ok",r[i].ok);
        if(r[i].ok) cJSON_AddNumberToObject(s,"celsius",r[i].millicelsius/1000.0); else cJSON_AddNullToObject(s,"celsius");
        cJSON_AddItemToArray(list,s);
    }
    return o;
}
static cJSON *config_json(const rf_management_settings_t *s) {
    cJSON *o=cJSON_CreateObject();
    cJSON_AddStringToObject(o,"hostname",s->hostname);
    cJSON_AddStringToObject(o,"fqdn",s->fqdn);
    cJSON_AddStringToObject(o,"ntp_server",s->ntp_server);
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
static esp_err_t session_get(httpd_req_t *req) {
    cJSON *o=cJSON_CreateObject();
    cJSON_AddBoolToObject(o,"authenticated",logged_in(req));
    return send_json(req,"200 OK",o);
}
static esp_err_t session_post(httpd_req_t *req) {
    if(!rf_login_allowed(now_ms())) return send_error(req,"429 Too Many Requests","Too many failed logins; try later");
    cJSON *body=read_json(req,SMALL_BODY);
    if(!body) return ESP_OK;
    const char *password=json_string(body,"password",RF_DEVICE_PASSWORD_MAX);
    bool match=false;
    if(password) rf_provision_verify_device_password(password,&match);
    delete_secret_json(body);
    rf_login_result(match,now_ms());
    if(!match) return send_error(req,"401 Unauthorized","Login failed");
    char token[RF_SESSION_TOKEN_LEN+1],cookie[RF_SESSION_TOKEN_LEN+80];
    rf_session_create(now_ms(),token);
    snprintf(cookie,sizeof(cookie),SESSION_COOKIE "=%s; Path=/; Secure; HttpOnly; SameSite=Strict",token);
    httpd_resp_set_hdr(req,"Set-Cookie",cookie);
    cJSON *o=cJSON_CreateObject();
    cJSON_AddBoolToObject(o,"authenticated",true);
    esp_err_t e=send_json(req,"200 OK",o);
    rf_secure_zero(token,sizeof(token));
    rf_secure_zero(cookie,sizeof(cookie));
    return e;
}
static esp_err_t session_delete(httpd_req_t *req) {
    if(logged_in(req)) rf_session_destroy();
    cJSON *o=cJSON_CreateObject();
    cJSON_AddBoolToObject(o,"authenticated",false);
    return send_json(req,"200 OK",o);
}
#define REQUIRE_LOGIN(req) do { if(!logged_in(req)) return send_error(req,"401 Unauthorized","Login required"); } while(0)
static esp_err_t status_handler(httpd_req_t *req) {
    REQUIRE_LOGIN(req);
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
    if(rf_network_get_ip(ip,sizeof(ip))==ESP_OK) cJSON_AddStringToObject(dev,"ipv4",ip); else cJSON_AddNullToObject(dev,"ipv4");
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
    cJSON_AddStringToObject(t,"ntp_server",s.ntp_server);
    cJSON *rf=cJSON_AddObjectToObject(o,"rf");
    cJSON_AddNumberToObject(rf,"filter",rf_filter_current());
    cJSON_AddItemToObject(o,"fan",fan_json());
    cJSON_AddItemToObject(o,"temperature",temperature_json());
    cJSON_AddItemToObject(o,"provisioning",provisioning_json());
    return send_json(req,"200 OK",o);
}
static esp_err_t filter_get(httpd_req_t *req) {
    REQUIRE_LOGIN(req);
    cJSON *o=cJSON_CreateObject();
    cJSON_AddNumberToObject(o,"filter",rf_filter_current());
    return send_json(req,"200 OK",o);
}
static esp_err_t filter_put(httpd_req_t *req) {
    REQUIRE_LOGIN(req);
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
    REQUIRE_LOGIN(req);
    return send_json(req,"200 OK",fan_json());
}
static esp_err_t fan_put(httpd_req_t *req) {
    REQUIRE_LOGIN(req);
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
    REQUIRE_LOGIN(req);
    return send_json(req,"200 OK",temperature_json());
}
static esp_err_t config_get(httpd_req_t *req) {
    REQUIRE_LOGIN(req);
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
    REQUIRE_LOGIN(req);
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
static bool edit_hostname(rf_management_settings_t *s, const cJSON *b) {
    return copy_field(s->hostname,sizeof(s->hostname),b,"hostname");
}
static bool edit_ntp(rf_management_settings_t *s, const cJSON *b) {
    return copy_field(s->ntp_server,sizeof(s->ntp_server),b,"server");
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
    REQUIRE_LOGIN(req);
    cJSON *body=read_json(req,SMALL_BODY);
    if(!body) return ESP_OK;
    const char *ssid=json_string(body,"ssid",RF_SSID_MAX),*password=json_string(body,"password",RF_WIFI_PASSWORD_MAX);
    esp_err_t e=ssid && password?rf_provision_set_wifi(ssid,password):ESP_ERR_INVALID_ARG;
    delete_secret_json(body);
    return stored_reply(req,e);
}
static esp_err_t tls_put(httpd_req_t *req) {
    REQUIRE_LOGIN(req);
    cJSON *body=read_json(req,TLS_BODY);
    if(!body) return ESP_OK;
    const char *cert=json_string(body,"certificate",RF_PEM_MAX),*key=json_string(body,"private_key",RF_PEM_MAX);
    esp_err_t e=cert && key?rf_provision_set_tls(cert,key):ESP_ERR_INVALID_ARG;
    delete_secret_json(body);
    return stored_reply(req,e);
}
static esp_err_t password_put(httpd_req_t *req) {
    REQUIRE_LOGIN(req);
    if(!rf_login_allowed(now_ms())) return send_error(req,"429 Too Many Requests","Too many failed attempts; try later");
    cJSON *body=read_json(req,SMALL_BODY);
    if(!body) return ESP_OK;
    const char *current=json_string(body,"current",RF_DEVICE_PASSWORD_MAX),*next=json_string(body,"new",RF_DEVICE_PASSWORD_MAX);
    bool match=false;
    if(current) rf_provision_verify_device_password(current,&match);
    rf_login_result(match,now_ms());
    esp_err_t e=ESP_ERR_INVALID_ARG;
    if(match && next) e=rf_provision_set_device_password(next);
    delete_secret_json(body);
    if(!match) return send_error(req,"401 Unauthorized","Current password is wrong");
    if(e==ESP_ERR_INVALID_ARG) return send_error(req,"400 Bad Request","New password must be 12 to 64 printable characters");
    if(e!=ESP_OK) return send_error(req,"500 Internal Server Error",esp_err_to_name(e));
    rf_session_destroy();
    cJSON *o=cJSON_CreateObject();
    cJSON_AddBoolToObject(o,"changed",true);
    return send_json(req,"200 OK",o);
}
static void reboot_now(void *arg) {
    esp_restart();
}
static esp_err_t reboot_post(httpd_req_t *req) {
    REQUIRE_LOGIN(req);
    if(!header_equals(req,"X-Confirm-Reboot","REBOOT")) return send_error(req,"412 Precondition Failed","Send X-Confirm-Reboot: REBOOT");
    const esp_timer_create_args_t args={.callback=reboot_now,.name="reboot"};
    if(!reboot_timer && esp_timer_create(&args,&reboot_timer)!=ESP_OK) return send_error(req,"500 Internal Server Error","Could not schedule reboot");
    esp_timer_stop(reboot_timer);
    if(esp_timer_start_once(reboot_timer,1000000)!=ESP_OK) return send_error(req,"500 Internal Server Error","Could not schedule reboot");
    cJSON *o=cJSON_CreateObject();
    cJSON_AddBoolToObject(o,"scheduled",true);
    cJSON_AddStringToObject(o,"action","reboot");
    return send_json(req,"202 Accepted",o);
}
static esp_err_t metrics_get(httpd_req_t *req) {
    char buf[1536];
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
    EMIT("# TYPE rf_heap_internal_free_bytes gauge\nrf_heap_internal_free_bytes %u\n",(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
#undef EMIT
    if(n>=sizeof(buf)) return send_error(req,"500 Internal Server Error","Metrics too large");
    httpd_resp_set_type(req,"text/plain; version=0.0.4");
    no_store(req);
    return httpd_resp_send(req,buf,(ssize_t)n);
}
static esp_err_t load_pem(nvs_handle_t nvs,const char *name,char **out,size_t *len) {
    *out=NULL; *len=0;
    esp_err_t err=nvs_get_blob(nvs,name,NULL,len);
    if(err!=ESP_OK || *len<32 || *len>16384) return ESP_ERR_INVALID_SIZE;
    char *buf=calloc(1,*len+1);
    if(!buf) return ESP_ERR_NO_MEM;
    size_t actual=*len;
    err=nvs_get_blob(nvs,name,buf,&actual);
    if(err!=ESP_OK || actual!=*len || buf[actual-1]!='\0') {
        memset(buf,0,*len+1); free(buf); return ESP_ERR_INVALID_STATE;
    }
    *out=buf; return ESP_OK;
}
esp_err_t web_start(void) {
    if(server) return ESP_ERR_INVALID_STATE;
    nvs_handle_t h;
    esp_err_t err=nvs_open("tls",NVS_READONLY,&h);
    if(err!=ESP_OK) { ESP_LOGW(TAG,"No TLS credentials provisioned; management listener disabled"); return err; }
    size_t cert_len=0,key_len=0;
    err=load_pem(h,"cert",&server_cert,&cert_len);
    if(err==ESP_OK) err=load_pem(h,"key",&server_key,&key_len);
    nvs_close(h);
    if(err!=ESP_OK) goto fail;
    const httpd_uri_t routes[]={
        {.uri="/",.method=HTTP_GET,.handler=index_handler},
        {.uri="/api/v1/health",.method=HTTP_GET,.handler=health_handler},
        {.uri="/metrics",.method=HTTP_GET,.handler=metrics_get},
        {.uri="/api/v1/session",.method=HTTP_GET,.handler=session_get},
        {.uri="/api/v1/session",.method=HTTP_POST,.handler=session_post},
        {.uri="/api/v1/session",.method=HTTP_DELETE,.handler=session_delete},
        {.uri="/api/v1/status",.method=HTTP_GET,.handler=status_handler},
        {.uri="/api/v1/rf/filter",.method=HTTP_GET,.handler=filter_get},
        {.uri="/api/v1/rf/filter",.method=HTTP_PUT,.handler=filter_put},
        {.uri="/api/v1/fan",.method=HTTP_GET,.handler=fan_get},
        {.uri="/api/v1/fan",.method=HTTP_PUT,.handler=fan_put},
        {.uri="/api/v1/sensors/temperature",.method=HTTP_GET,.handler=temperature_get},
        {.uri="/api/v1/config",.method=HTTP_GET,.handler=config_get},
        {.uri="/api/v1/config/hostname",.method=HTTP_PUT,.handler=hostname_put},
        {.uri="/api/v1/config/ntp",.method=HTTP_PUT,.handler=ntp_put},
        {.uri="/api/v1/config/network",.method=HTTP_PUT,.handler=network_put},
        {.uri="/api/v1/config/wifi",.method=HTTP_PUT,.handler=wifi_put},
        {.uri="/api/v1/config/password",.method=HTTP_PUT,.handler=password_put},
        {.uri="/api/v1/config/tls",.method=HTTP_PUT,.handler=tls_put},
        {.uri="/api/v1/system/reboot",.method=HTTP_POST,.handler=reboot_post},
    };
    httpd_ssl_config_t cfg=HTTPD_SSL_CONFIG_DEFAULT();
    cfg.servercert=(const unsigned char*)server_cert;
    cfg.servercert_len=cert_len;
    cfg.prvtkey_pem=(const unsigned char*)server_key;
    cfg.prvtkey_len=key_len;
    cfg.httpd.max_uri_handlers=sizeof(routes)/sizeof(routes[0]);
    cfg.httpd.lru_purge_enable=true;
    err=httpd_ssl_start(&server,&cfg);
    if(err!=ESP_OK) goto fail;
    for(size_t i=0;i<sizeof(routes)/sizeof(routes[0]) && err==ESP_OK;i++) err=httpd_register_uri_handler(server,&routes[i]);
    if(err==ESP_OK) return ESP_OK;
    httpd_ssl_stop(server); server=NULL;
fail:
    if(server_key) { memset(server_key,0,key_len); free(server_key); server_key=NULL; }
    free(server_cert); server_cert=NULL;
    ESP_LOGE(TAG,"HTTPS management disabled: %s",esp_err_to_name(err));
    return err;
}
