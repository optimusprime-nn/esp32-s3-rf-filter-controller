#include "esp_https_server.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs.h"
#include "esp_timer.h"
#include "console_line.h"
#include "filter.h"
#include "management_settings.h"
#include "provisioning.h"
#include "session.h"
#include "time_service.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static const char *TAG="web_tls";
static httpd_handle_t server;
static char *server_cert,*server_key;
#define SESSION_COOKIE "rf_session"
static int64_t now_ms(void) {
    return esp_timer_get_time()/1000;
}
static esp_err_t reply(httpd_req_t *req, const char *status, const char *body) {
    httpd_resp_set_status(req,status);
    httpd_resp_set_type(req,"application/json");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    return httpd_resp_sendstr(req,body);
}
/* Browsers cannot send this header cross-origin without a preflight, which is never answered. */
static bool same_origin_request(httpd_req_t *req) {
    return httpd_req_get_hdr_value_len(req,"X-RF-Request")>0;
}
static bool logged_in(httpd_req_t *req) {
    char token[RF_SESSION_TOKEN_LEN+1];
    size_t n=sizeof(token);
    return httpd_req_get_cookie_val(req,SESSION_COOKIE,token,&n)==ESP_OK && rf_session_check(token,now_ms());
}
static bool read_body(httpd_req_t *req, char *buf, size_t cap) {
    if(!req->content_len || req->content_len>=cap) return false;
    size_t got=0;
    while(got<req->content_len) {
        int r=httpd_req_recv(req,buf+got,req->content_len-got);
        if(r==HTTPD_SOCK_ERR_TIMEOUT) continue;
        if(r<=0) return false;
        got+=(size_t)r;
    }
    buf[got]='\0';
    return true;
}
static esp_err_t login_handler(httpd_req_t *req) {
    if(!same_origin_request(req)) return reply(req,"403 Forbidden","{\"error\":\"forbidden\"}");
    if(!rf_login_allowed(now_ms())) return reply(req,"429 Too Many Requests","{\"error\":\"try later\"}");
    char password[RF_DEVICE_PASSWORD_MAX+1];
    bool match=false;
    if(read_body(req,password,sizeof(password))) rf_provision_verify_device_password(password,&match);
    rf_secure_zero(password,sizeof(password));
    rf_login_result(match,now_ms());
    if(!match) return reply(req,"401 Unauthorized","{\"error\":\"login failed\"}");
    char token[RF_SESSION_TOKEN_LEN+1],cookie[RF_SESSION_TOKEN_LEN+80];
    rf_session_create(now_ms(),token);
    snprintf(cookie,sizeof(cookie),SESSION_COOKIE "=%s; Path=/; Secure; HttpOnly; SameSite=Strict",token);
    httpd_resp_set_hdr(req,"Set-Cookie",cookie);
    esp_err_t e=reply(req,"200 OK","{\"ok\":true}");
    rf_secure_zero(token,sizeof(token));
    rf_secure_zero(cookie,sizeof(cookie));
    return e;
}
static esp_err_t logout_handler(httpd_req_t *req) {
    if(!same_origin_request(req)) return reply(req,"403 Forbidden","{\"error\":\"forbidden\"}");
    if(logged_in(req)) rf_session_destroy();
    return reply(req,"200 OK","{\"ok\":true}");
}
static esp_err_t filter_handler(httpd_req_t *req) {
    if(!same_origin_request(req)) return reply(req,"403 Forbidden","{\"error\":\"forbidden\"}");
    if(!logged_in(req)) return reply(req,"401 Unauthorized","{\"error\":\"login required\"}");
    char body[4];
    if(!read_body(req,body,sizeof(body)) || strlen(body)!=1 || body[0]<'1' || body[0]>'4')
        return reply(req,"400 Bad Request","{\"error\":\"filter must be 1 to 4\"}");
    if(rf_filter_select((uint8_t)(body[0]-'0'))!=ESP_OK)
        return reply(req,"500 Internal Server Error","{\"error\":\"switch failed\"}");
    return reply(req,"200 OK","{\"ok\":true}");
}
static esp_err_t status_handler(httpd_req_t *req) {
    char body[320];
    time_t now=time(NULL);
    rf_management_settings_t settings;
    rf_provision_status_t provisioned;
    rf_provision_get_status(&provisioned);
    if(rf_settings_load(&settings)!=ESP_OK)
        return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Configuration unavailable");
    snprintf(body,sizeof(body),
        "{\"system\":\"rf-controller\",\"uptime_ms\":%lu,\"filter\":%u,\"control_enabled\":%s,\"dhcp\":%s,\"ntp_synchronized\":%s,\"unix_time\":%lld,\"acme_enabled\":%s}",
        (unsigned long)esp_log_timestamp(),(unsigned)rf_filter_current(),provisioned.device_password?"true":"false",settings.dhcp?"true":"false",
        rf_time_service_is_synchronized()?"true":"false",(long long)now,
        settings.acme_enabled?"true":"false");
    httpd_resp_set_type(req,"application/json");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    return httpd_resp_sendstr(req,body);
}
static esp_err_t index_handler(httpd_req_t *req) {
    const char *html="<!doctype html><html lang='en'><meta charset='utf-8'><meta name='viewport' content='width=device-width'><title>RF Controller</title><h1>RF Controller</h1><form id='login'><input id='pw' type='password' autocomplete='current-password' aria-label='Device password'> <button>Log in</button></form><p><button data-p='1'>Filter 1</button> <button data-p='2'>Filter 2</button> <button data-p='3'>Filter 3</button> <button data-p='4'>Filter 4</button> <button id='out'>Log out</button></p><p id='msg' role='status'></p><pre id='status'>Loading...</pre><script>const $=i=>document.getElementById(i);const post=(u,b)=>fetch(u,{method:'POST',headers:{'X-RF-Request':'1'},body:b});const show=()=>fetch('/api/status',{cache:'no-store'}).then(r=>r.json()).then(j=>$('status').textContent=JSON.stringify(j,null,2)).catch(()=>$('status').textContent='Unavailable');const done=r=>{$('msg').textContent=r.ok?'OK':'Failed: '+r.status;show()};$('login').onsubmit=e=>{e.preventDefault();post('/api/login',$('pw').value).then(done);$('pw').value=''};$('out').onclick=()=>post('/api/logout','').then(done);document.querySelectorAll('[data-p]').forEach(b=>b.onclick=()=>post('/api/filter',b.dataset.p).then(done));show()</script></html>";
    httpd_resp_set_type(req,"text/html; charset=utf-8");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    return httpd_resp_sendstr(req,html);
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
    httpd_ssl_config_t cfg=HTTPD_SSL_CONFIG_DEFAULT();
    cfg.servercert=(const unsigned char*)server_cert;
    cfg.servercert_len=cert_len;
    cfg.prvtkey_pem=(const unsigned char*)server_key;
    cfg.prvtkey_len=key_len;
    err=httpd_ssl_start(&server,&cfg);
    if(err!=ESP_OK) goto fail;
    const httpd_uri_t routes[]={
        {.uri="/api/status",.method=HTTP_GET,.handler=status_handler},
        {.uri="/",.method=HTTP_GET,.handler=index_handler},
        {.uri="/api/login",.method=HTTP_POST,.handler=login_handler},
        {.uri="/api/logout",.method=HTTP_POST,.handler=logout_handler},
        {.uri="/api/filter",.method=HTTP_POST,.handler=filter_handler},
    };
    for(size_t i=0;i<sizeof(routes)/sizeof(routes[0]) && err==ESP_OK;i++) err=httpd_register_uri_handler(server,&routes[i]);
    if(err==ESP_OK) return ESP_OK;
    httpd_ssl_stop(server); server=NULL;
fail:
    if(server_key) { memset(server_key,0,key_len); free(server_key); server_key=NULL; }
    free(server_cert); server_cert=NULL;
    ESP_LOGE(TAG,"HTTPS management disabled: %s",esp_err_to_name(err));
    return err;
}
