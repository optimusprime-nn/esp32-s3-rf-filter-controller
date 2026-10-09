#include "esp_https_server.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs.h"
#include "management_settings.h"
#include "time_service.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static const char *TAG="web_tls";
static httpd_handle_t server;
static char *server_cert,*server_key;
static esp_err_t status_handler(httpd_req_t *req) {
    char body[320];
    time_t now=time(NULL);
    rf_management_settings_t settings;
    if(rf_settings_load(&settings)!=ESP_OK)
        return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Configuration unavailable");
    snprintf(body,sizeof(body),
        "{\"system\":\"rf-controller\",\"uptime_ms\":%lu,\"control_enabled\":false,\"dhcp\":%s,\"ntp_synchronized\":%s,\"unix_time\":%lld,\"acme_enabled\":%s}",
        (unsigned long)esp_log_timestamp(),settings.dhcp?"true":"false",
        rf_time_service_is_synchronized()?"true":"false",(long long)now,
        settings.acme_enabled?"true":"false");
    httpd_resp_set_type(req,"application/json");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    return httpd_resp_sendstr(req,body);
}
static esp_err_t index_handler(httpd_req_t *req) {
    const char *html="<!doctype html><html lang='en'><meta charset='utf-8'><meta name='viewport' content='width=device-width'><title>RF Controller</title><h1>RF Controller</h1><p>Read-only diagnostic mode. Remote control and configuration are disabled pending authentication.</p><pre id='status'>Loading...</pre><script>fetch('/api/status',{cache:'no-store'}).then(r=>r.json()).then(j=>document.getElementById('status').textContent=JSON.stringify(j,null,2)).catch(()=>document.getElementById('status').textContent='Unavailable')</script></html>";
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
    httpd_uri_t status={.uri="/api/status",.method=HTTP_GET,.handler=status_handler};
    httpd_uri_t index={.uri="/",.method=HTTP_GET,.handler=index_handler};
    err=httpd_register_uri_handler(server,&status);
    if(err==ESP_OK) err=httpd_register_uri_handler(server,&index);
    if(err==ESP_OK) return ESP_OK;
    httpd_ssl_stop(server); server=NULL;
fail:
    if(server_key) { memset(server_key,0,key_len); free(server_key); server_key=NULL; }
    free(server_cert); server_cert=NULL;
    ESP_LOGE(TAG,"HTTPS management disabled: %s",esp_err_to_name(err));
    return err;
}
