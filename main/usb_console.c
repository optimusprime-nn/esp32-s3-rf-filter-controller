#include "console_line.h"
#include "fan.h"
#include "filter.h"
#include "provisioning.h"
#include "ownership.h"
#include "bootstrap_tls.h"
#include "temp_sensor.h"
#include "weather.h"
#include "sdr_power.h"
#include "acme.h"
#include "driver/usb_serial_jtag.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char *TAG="usb_console";
static void show_sdr(void);
#define PEM_LINE_MAX 128
extern esp_err_t rf_network_get_ip(char *out, size_t len);
extern bool web_certificate_sha256(char out[65], bool *bootstrap);
static void put(const char *s) {
    usb_serial_jtag_write_bytes(s,strlen(s),pdMS_TO_TICKS(100));
}
/* Returns true for a complete line. Secrets are never echoed back to the host. */
/* Carries CR state between lines, so a terminal sending CR+LF for Enter (such as
   miniterm) doesn't leave an LF that ends the next line empty. */
static bool s_after_cr;
static bool read_line(char *buf, size_t cap, bool echo) {
    rf_line_t line;
    rf_line_init(&line,buf,cap,s_after_cr);
    for(;;) {
        unsigned char c;
        if(usb_serial_jtag_read_bytes(&c,1,portMAX_DELAY)!=1) continue;
        size_t before=line.len;
        bool erased;
        rf_line_result_t r=rf_line_feed(&line,c,&erased);
        s_after_cr=line.last_cr;
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
            if(read_pem(key,cap,false)) {
                esp_err_t e=rf_provision_set_tls(cert,key);
                if(e==ESP_OK) rf_acme_forget_certificate();
                report(e);
            }
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
static void show_fan(void) {
    char line[48];
    int32_t rpm=rf_fan_rpm_now();
    if(rpm<0) snprintf(line,sizeof(line),"Fan: %u%%\r\n",(unsigned)rf_fan_percent());
    else snprintf(line,sizeof(line),"Fan: %u%%, %ld RPM\r\n",(unsigned)rf_fan_percent(),(long)rpm);
    put(line);
}
static void set_fan(const char *arg) {
    char *end;
    long p=strtol(arg,&end,10);
    if(!*arg || *end || p<0 || p>100) { put("ERROR: fan speed must be 0 to 100\r\n"); return; }
    esp_err_t e=rf_fan_set((uint8_t)p);
    if(e==ESP_OK) show_fan();
    else report(e);
}
static void show_temps(void) {
    rf_temp_reading_t r[RF_TEMP_MAX_SENSORS];
    size_t n=rf_temp_snapshot(r,RF_TEMP_MAX_SENSORS);
    char line[80];
    if(!n) put("Temperature: no sensors\r\n");
    for(size_t i=0;i<n;i++) {
        if(r[i].ok && r[i].has_humidity) snprintf(line,sizeof(line),"Temperature %s: %s%ld.%01ld C, %ld.%01ld%% humidity\r\n",r[i].id,r[i].millicelsius<0?"-":"",
            labs(r[i].millicelsius)/1000,labs(r[i].millicelsius)%1000/100,(long)r[i].millipercent_rh/1000,(long)r[i].millipercent_rh%1000/100);
        else if(r[i].ok) snprintf(line,sizeof(line),"Temperature %s: %s%ld.%01ld C\r\n",r[i].id,r[i].millicelsius<0?"-":"",labs(r[i].millicelsius)/1000,labs(r[i].millicelsius)%1000/100);
        else snprintf(line,sizeof(line),"Temperature %s: not responding\r\n",r[i].id);
        put(line);
    }
}
static void show_weather(void) {
    rf_weather_status_t w;
    rf_weather_status(&w);
    char line[128];
    if(w.receiver!=RF_WEATHER_RUNNING) {
        snprintf(line,sizeof(line),"Weather: receiver %s\r\n",rf_weather_receiver_name(w.receiver));
        put(line);
        return;
    }
    if(w.sensor_id_filter<0) snprintf(line,sizeof(line),"Weather: %lu.%03lu MHz, any station, %lu readings, %lu rejected, %lu ignored\r\n",
        (unsigned long)(w.frequency_khz/1000),(unsigned long)(w.frequency_khz%1000),(unsigned long)w.packets_ok,(unsigned long)w.packets_rejected,(unsigned long)w.packets_ignored);
    else snprintf(line,sizeof(line),"Weather: %lu.%03lu MHz, station %04lx only, %lu readings, %lu rejected, %lu ignored\r\n",
        (unsigned long)(w.frequency_khz/1000),(unsigned long)(w.frequency_khz%1000),(unsigned long)w.sensor_id_filter,(unsigned long)w.packets_ok,(unsigned long)w.packets_rejected,(unsigned long)w.packets_ignored);
    put(line);
    for(size_t i=0;i<w.heard_count;i++) {
        snprintf(line,sizeof(line),"Heard station %04x: %d dBm, %lld s ago\r\n",w.heard[i].id,w.heard[i].rssi_dbm,(long long)(w.heard[i].age_ms/1000));
        put(line);
    }
    if(!w.have_reading) { put("No reading yet; the station sends every 20 s\r\n"); return; }
    const rf_vevor_reading_t *r=&w.last;
    snprintf(line,sizeof(line),"Station %04x, %lld s ago, %d dBm, battery %s\r\n",r->id,(long long)(w.age_ms/1000),w.rssi_dbm,r->battery_low?"low":"ok");
    put(line);
    snprintf(line,sizeof(line),"%s%d.%d C, %u%% humidity, UV %d, %lu lux\r\n",r->temp_dc<0?"-":"",abs(r->temp_dc)/10,abs(r->temp_dc)%10,
        (unsigned)r->humidity,(int)r->uv_index,(unsigned long)r->light_lux);
    put(line);
    snprintf(line,sizeof(line),"Wind %.1f km/h, gust %.1f km/h, from %u deg; rain %.1f mm\r\n",
        rf_vevor_wind_kmh(r->wind_raw),rf_vevor_gust_kmh(r->gust_raw),(unsigned)r->wind_dir_deg,rf_vevor_rain_mm(r->rain_raw));
    put(line);
}
static void set_station(const char *arg) {
    long id;
    char *end;
    if(!strcmp(arg,"any")) id=-1;
    else { id=strtol(arg,&end,16); if(strlen(arg)!=4 || *end) { put("ERROR: give a 4-digit hex station ID, or any\r\n"); return; } }
    esp_err_t e=rf_weather_set_station((int32_t)id);
    if(e==ESP_OK) show_weather();
    else if(e==ESP_ERR_NOT_SUPPORTED || e==ESP_ERR_INVALID_STATE) put("ERROR: weather receiver is not running\r\n");
    else { put("ERROR: "); put(esp_err_to_name(e)); put("\r\n"); }
}
static void show_enrollment(void) {
    char hex[65];
    if(rf_ownership_claimed()) { put("Already claimed. Management needs a client certificate from your management CA.\r\n"); return; }
    put("One-time management enrollment code: "); put(rf_ownership_code()); put("\r\n");
    if(rf_bootstrap_tls_sha256(hex)==ESP_OK) { put("Bootstrap certificate SHA-256: "); put(hex); put("\r\n"); }
    put("A certificate stored with set-tls is used instead once one is stored.\r\n");
}
static void show_status(void) {
    rf_provision_status_t s;
    char ip[16];
    rf_provision_get_status(&s);
    put("Firmware: "); put(esp_app_get_description()->version); put("\r\n");
    show_filter();
    show_fan();
    show_temps();
    show_weather();
    put("IPv4 address: "); put(rf_network_get_ip(ip,sizeof(ip))==ESP_OK?ip:"none"); put("\r\n");
    put("Wi-Fi credentials: "); put(s.wifi?"provisioned\r\n":"missing\r\n");
    put("Management: "); put(rf_ownership_claimed()?"claimed (mTLS)\r\n":"unclaimed; run enrollment\r\n");
    put("TLS certificate and key: "); put(s.tls?"provisioned\r\n":"missing\r\n");
    /* Same wording as the boot log, so the setup tool can pin HTTPS without resetting the board. */
    char hex[65];
    bool bootstrap;
    if(web_certificate_sha256(hex,&bootstrap)) {
        put(bootstrap?"Bootstrap certificate SHA-256: ":"Server certificate SHA-256: "); put(hex); put("\r\n");
    }
    show_sdr();
}
static void show_sdr(void) {
    rf_sdr_status_t s;
    rf_sdr_status(&s);
    char b[96];
    snprintf(b,sizeof(b),"SDR: %s; %lu reboots, %lu power cycles since boot\r\n",rf_sdr_state_name(s.state),
        (unsigned long)s.soft_resets,(unsigned long)s.hard_resets);
    put(b);
}
static void sdr_command(bool hard) {
    esp_err_t e=hard?rf_sdr_hard_reset():rf_sdr_soft_reset();
    if(e==ESP_OK) put(hard?"SDR power cut; it comes back by itself\r\n":"SDR reboot requested\r\n");
    else if(e==ESP_ERR_INVALID_STATE) put("ERROR: an SDR reset is in progress or just finished; try again shortly\r\n");
    else { put("ERROR: "); put(esp_err_to_name(e)); put("\r\n"); }
}
static void console_task(void *arg) {
    char cmd[32];
    for(;;) {
        put("rf> ");
        if(!read_line(cmd,sizeof(cmd),true) || !cmd[0]) continue;
        if(!strcmp(cmd,"status")) show_status();
        else if(!strcmp(cmd,"filter")) show_filter();
        else if(!strncmp(cmd,"filter ",7)) select_port(cmd+7);
        else if(!strcmp(cmd,"fan")) show_fan();
        else if(!strcmp(cmd,"temps")) show_temps();
        else if(!strcmp(cmd,"weather")) show_weather();
        else if(!strncmp(cmd,"weather-station ",16)) set_station(cmd+16);
        else if(!strncmp(cmd,"fan ",4)) set_fan(cmd+4);
        else if(!strcmp(cmd,"set-wifi")) set_wifi();
        else if(!strcmp(cmd,"enrollment")) show_enrollment();
        else if(!strcmp(cmd,"set-tls")) set_tls();
        else if(!strcmp(cmd,"sdr")) show_sdr();
        else if(!strcmp(cmd,"sdr-reboot")) sdr_command(false);
        else if(!strcmp(cmd,"sdr-power-cycle")) sdr_command(true);
        else put("Commands: status, filter [1-4], fan [0-100], temps, weather, weather-station <id|any>, sdr, sdr-reboot, sdr-power-cycle, enrollment, set-wifi, set-tls. Ctrl-C cancels.\r\n");
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
