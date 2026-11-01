#include "console_line.h"
#include "filter.h"
#include "provisioning.h"
#include "session.h"
#include "driver/usb_serial_jtag.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char *TAG="usb_console";
#define PEM_LINE_MAX 128
extern esp_err_t rf_network_get_ip(char *out, size_t len);
static void put(const char *s) {
    usb_serial_jtag_write_bytes(s,strlen(s),pdMS_TO_TICKS(100));
}
/* Returns true for a complete line. Secrets are never echoed back to the host. */
static bool read_line(char *buf, size_t cap, bool echo) {
    rf_line_t line;
    rf_line_init(&line,buf,cap);
    for(;;) {
        unsigned char c;
        if(usb_serial_jtag_read_bytes(&c,1,portMAX_DELAY)!=1) continue;
        size_t before=line.len;
        bool erased;
        rf_line_result_t r=rf_line_feed(&line,c,&erased);
        if(r==RF_LINE_DONE) { put("\r\n"); return true; }
        if(r==RF_LINE_CANCEL) { put("\r\nCancelled\r\n"); return false; }
        if(r==RF_LINE_OVERFLOW) { put("\r\nERROR: line too long\r\n"); return false; }
        if(echo && erased) put("\b \b");
        if(echo && line.len>before) usb_serial_jtag_write_bytes(&c,1,pdMS_TO_TICKS(100));
    }
}
static void report(esp_err_t e) {
    if(e==ESP_OK) put("OK; takes effect at next reset\r\n");
    else if(e==ESP_ERR_INVALID_ARG) put("ERROR: value rejected\r\n");
    else { put("ERROR: "); put(esp_err_to_name(e)); put("\r\n"); }
}
static void set_wifi(void) {
    char ssid[RF_SSID_MAX+1],password[RF_WIFI_PASSWORD_MAX+1];
    put("SSID: ");
    if(!read_line(ssid,sizeof(ssid),true)) return;
    put("Password (not echoed): ");
    if(read_line(password,sizeof(password),false)) report(rf_provision_set_wifi(ssid,password));
    rf_secure_zero(password,sizeof(password));
}
static void set_password(void) {
    char first[RF_DEVICE_PASSWORD_MAX+1],second[RF_DEVICE_PASSWORD_MAX+1];
    put("New device password, 12 to 64 characters (not echoed): ");
    bool ok=read_line(first,sizeof(first),false);
    if(ok) { put("Repeat: "); ok=read_line(second,sizeof(second),false); }
    if(ok) {
        if(strcmp(first,second)) put("ERROR: passwords differ\r\n");
        else {
            esp_err_t e=rf_provision_set_device_password(first);
            if(e==ESP_OK) rf_session_destroy();
            report(e);
        }
    }
    rf_secure_zero(first,sizeof(first));
    rf_secure_zero(second,sizeof(second));
}
/* Reads PEM lines until the END marker, normalising line endings to LF. */
static bool read_pem(char *out, size_t cap, bool echo) {
    char line[PEM_LINE_MAX];
    size_t len=0;
    bool ok=false;
    out[0]='\0';
    for(;;) {
        if(!read_line(line,sizeof(line),echo)) break;
        size_t n=strlen(line);
        if(len+n+2>cap) { put("ERROR: PEM too long\r\n"); break; }
        memcpy(out+len,line,n);
        len+=n;
        out[len++]='\n';
        out[len]='\0';
        if(!strncmp(line,"-----END ",9)) { ok=true; break; }
    }
    rf_secure_zero(line,sizeof(line));
    return ok;
}
static void set_tls(void) {
    size_t cap=RF_PEM_MAX+1;
    char *cert=malloc(cap),*key=malloc(cap);
    if(!cert || !key) put("ERROR: out of memory\r\n");
    else {
        put("Paste certificate PEM:\r\n");
        if(read_pem(cert,cap,true)) {
            put("Paste private key PEM (not echoed):\r\n");
            if(read_pem(key,cap,false)) report(rf_provision_set_tls(cert,key));
        }
    }
    if(key) { rf_secure_zero(key,cap); free(key); }
    free(cert);
}
static void show_filter(void) {
    char line[32];
    snprintf(line,sizeof(line),"Filter: %u\r\n",(unsigned)rf_filter_current());
    put(line);
}
static void select_port(const char *arg) {
    if(strlen(arg)!=1 || arg[0]<'1' || arg[0]>'4') { put("ERROR: filter must be 1 to 4\r\n"); return; }
    esp_err_t e=rf_filter_select((uint8_t)(arg[0]-'0'));
    if(e==ESP_OK) show_filter();
    else report(e);
}
static void show_status(void) {
    rf_provision_status_t s;
    char ip[16];
    rf_provision_get_status(&s);
    put("Firmware: "); put(esp_app_get_description()->version); put("\r\n");
    show_filter();
    put("IPv4 address: "); put(rf_network_get_ip(ip,sizeof(ip))==ESP_OK?ip:"none"); put("\r\n");
    put("Wi-Fi credentials: "); put(s.wifi?"provisioned\r\n":"missing\r\n");
    put("Device password: "); put(s.device_password?"provisioned\r\n":"missing\r\n");
    put("TLS certificate and key: "); put(s.tls?"provisioned\r\n":"missing\r\n");
}
static void console_task(void *arg) {
    char cmd[32];
    for(;;) {
        put("rf> ");
        if(!read_line(cmd,sizeof(cmd),true) || !cmd[0]) continue;
        if(!strcmp(cmd,"status")) show_status();
        else if(!strcmp(cmd,"filter")) show_filter();
        else if(!strncmp(cmd,"filter ",7)) select_port(cmd+7);
        else if(!strcmp(cmd,"set-wifi")) set_wifi();
        else if(!strcmp(cmd,"set-password")) set_password();
        else if(!strcmp(cmd,"set-tls")) set_tls();
        else put("Commands: status, filter [1-4], set-wifi, set-password, set-tls. Ctrl-C cancels.\r\n");
    }
}
esp_err_t rf_usb_console_start(void) {
    usb_serial_jtag_driver_config_t cfg=USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size=1024;
    esp_err_t e=usb_serial_jtag_driver_install(&cfg);
    if(e!=ESP_OK) return e;
    if(xTaskCreate(console_task,"usb_console",8192,NULL,2,NULL)!=pdPASS) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG,"USB provisioning console ready");
    return ESP_OK;
}
