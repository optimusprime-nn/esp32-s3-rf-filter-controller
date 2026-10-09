#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_netif.h"
#include "management_settings.h"
#include "time_service.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
static esp_err_t status_handler(httpd_req_t *req) {
    char body[320];
    time_t now=time(NULL);
    rf_management_settings_t settings;
    esp_err_t e=rf_settings_load(&settings);
    if(e!=ESP_OK) return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Configuration unavailable");
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
    const char *html="<!doctype html><html lang='en'><meta charset='utf-8'><meta name='viewport' content='width=device-width'><title>RF Controller</title><h1>RF Controller</h1><p>Read-only diagnostic mode. Remote control and configuration are disabled pending authenticated HTTPS.</p><pre id='status'>Loading...</pre><script>fetch('/api/status',{cache:'no-store'}).then(r=>r.json()).then(j=>document.getElementById('status').textContent=JSON.stringify(j,null,2)).catch(()=>document.getElementById('status').textContent='Unavailable')</script></html>";
    httpd_resp_set_type(req,"text/html; charset=utf-8");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    return httpd_resp_sendstr(req,html);
}
esp_err_t web_start(void) {
    httpd_handle_t server=NULL;
    httpd_config_t config=HTTPD_DEFAULT_CONFIG();
    esp_err_t err=httpd_start(&server,&config);
    if(err!=ESP_OK) return err;
    httpd_uri_t status={.uri="/api/status",.method=HTTP_GET,.handler=status_handler};
    httpd_uri_t index={.uri="/",.method=HTTP_GET,.handler=index_handler};
    err=httpd_register_uri_handler(server,&status);
    if(err!=ESP_OK) return err;
    return httpd_register_uri_handler(server,&index);
}
