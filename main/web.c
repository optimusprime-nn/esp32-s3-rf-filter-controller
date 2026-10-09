#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_err.h"
#include <stdio.h>
#include <string.h>
/* Read-only diagnostic API only. No unauthenticated mutations. */
static esp_err_t status_handler(httpd_req_t *req) {
    char body[128];
    snprintf(body,sizeof(body),"{\"system\":\"rf-controller\",\"uptime_ms\":%lu,\"control_enabled\":false}",(unsigned long)(esp_log_timestamp()));
    httpd_resp_set_type(req,"application/json");
    return httpd_resp_sendstr(req,body);
}
esp_err_t web_start(void) {
    httpd_handle_t server=NULL;
    httpd_config_t config=HTTPD_DEFAULT_CONFIG();
    esp_err_t err=httpd_start(&server,&config);
    if(err!=ESP_OK) return err;
    httpd_uri_t status={.uri="/api/status",.method=HTTP_GET,.handler=status_handler,.user_ctx=NULL};
    return httpd_register_uri_handler(server,&status);
}
