#include "weather_feed.h"
#include "weather.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
static const char *TAG="wxfeed";
#define REPEAT_MS 30000
static int clients[RF_WEATHER_FEED_MAX_CLIENTS]={-1,-1,-1,-1};
static volatile size_t client_count;
static volatile int port;

static int64_t now_ms(void) { return esp_timer_get_time()/1000; }
static double r1(double v) { return round(v*10)/10; }

#if CONFIG_RF_WEATHER_FEED_ENABLE
/* One line, newline-terminated. Returns its length. */
static int format_line(char *buf, size_t len, const rf_weather_status_t *w) {
    if(!w->have_reading)
        return snprintf(buf,len,"{\"type\":\"waiting\",\"receiver\":\"%s\",\"frequency_mhz\":%.3f}\n",
            rf_weather_receiver_name(w->receiver),w->frequency_khz/1000.0);
    const rf_vevor_reading_t *r=&w->last;
    long age=(long)(w->age_ms/1000);
    return snprintf(buf,len,
        "{\"type\":\"reading\",\"model\":\"VEVOR 7-in-1\",\"station\":\"%04x\",\"channel\":%u,\"age_s\":%ld,\"stale\":%s,"
        "\"rssi_dbm\":%d,\"battery_ok\":%s,\"temperature_c\":%.1f,\"humidity_percent\":%u,"
        "\"wind_avg_kmh\":%.1f,\"wind_gust_kmh\":%.1f,\"wind_dir_deg\":%u,\"rain_mm\":%.1f,"
        "\"uv_index\":%d,\"light_lux\":%lu}\n",
        r->id,(unsigned)r->channel,age,age>RF_WEATHER_STALE_S?"true":"false",
        w->rssi_dbm,r->battery_low?"false":"true",r->temp_dc/10.0,(unsigned)r->humidity,
        r1(rf_vevor_wind_kmh(r->wind_raw)),r1(rf_vevor_gust_kmh(r->gust_raw)),(unsigned)r->wind_dir_deg,
        r1(rf_vevor_rain_mm(r->rain_raw)),(int)r->uv_index,(unsigned long)r->light_lux);
}
static void drop(size_t i) {
    close(clients[i]);
    clients[i]=-1;
    client_count--;
}
/* A client that can't take a line at once is too slow or gone: drop it rather than block. */
static void send_to(size_t i, const char *line, int n) {
    if(clients[i]<0) return;
    if(send(clients[i],line,(size_t)n,MSG_DONTWAIT)!=n) {
        ESP_LOGI(TAG,"Client dropped (%s)",errno==EAGAIN?"too slow":"disconnected");
        drop(i);
    }
}
static void feed_task(void *arg) {
    int ls=socket(AF_INET,SOCK_STREAM,IPPROTO_IP);
    if(ls<0) { ESP_LOGE(TAG,"socket: errno %d",errno); port=0; vTaskDelete(NULL); return; }
    int yes=1;
    setsockopt(ls,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
    struct sockaddr_in addr={.sin_family=AF_INET,.sin_port=htons(CONFIG_RF_WEATHER_FEED_PORT),.sin_addr.s_addr=htonl(INADDR_ANY)};
    if(bind(ls,(struct sockaddr *)&addr,sizeof(addr))<0 || listen(ls,2)<0) {
        ESP_LOGE(TAG,"Can't listen on TCP %d: errno %d",CONFIG_RF_WEATHER_FEED_PORT,errno);
        close(ls); port=0; vTaskDelete(NULL); return;
    }
    ESP_LOGI(TAG,"Weather feed on TCP port %d",CONFIG_RF_WEATHER_FEED_PORT);
    uint32_t sent_ok=0;
    int64_t last_sent=0;
    char line[512];
    for(;;) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(ls,&rd);
        int maxfd=ls;
        for(size_t i=0;i<RF_WEATHER_FEED_MAX_CLIENTS;i++) if(clients[i]>=0) { FD_SET(clients[i],&rd); if(clients[i]>maxfd) maxfd=clients[i]; }
        struct timeval tv={.tv_sec=1};
        int ready=select(maxfd+1,&rd,NULL,NULL,&tv);
        rf_weather_status_t w;
        rf_weather_status(&w);
        if(ready>0 && FD_ISSET(ls,&rd)) {
            struct sockaddr_in peer;
            socklen_t plen=sizeof(peer);
            int c=accept(ls,(struct sockaddr *)&peer,&plen);
            if(c>=0) {
                size_t slot=RF_WEATHER_FEED_MAX_CLIENTS;
                for(size_t i=0;i<RF_WEATHER_FEED_MAX_CLIENTS;i++) if(clients[i]<0) { slot=i; break; }
                char ip[16];
                inet_ntoa_r(peer.sin_addr,ip,sizeof(ip));
                if(slot==RF_WEATHER_FEED_MAX_CLIENTS) {
                    ESP_LOGW(TAG,"Refused %s: already %d clients",ip,RF_WEATHER_FEED_MAX_CLIENTS);
                    close(c);
                } else {
                    int one=1;
                    setsockopt(c,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));
                    setsockopt(c,SOL_SOCKET,SO_KEEPALIVE,&one,sizeof(one));
                    clients[slot]=c;
                    client_count++;
                    ESP_LOGI(TAG,"Client %s connected",ip);
                    int n=format_line(line,sizeof(line),&w);
                    if(n>0 && (size_t)n<sizeof(line)) send_to(slot,line,n);
                }
            }
        }
        /* Clients only listen: discard what they send, and notice when they hang up. */
        for(size_t i=0;ready>0 && i<RF_WEATHER_FEED_MAX_CLIENTS;i++) {
            if(clients[i]<0 || !FD_ISSET(clients[i],&rd)) continue;
            char junk[64];
            int n=recv(clients[i],junk,sizeof(junk),MSG_DONTWAIT);
            if(n==0 || (n<0 && errno!=EAGAIN && errno!=EWOULDBLOCK)) { ESP_LOGI(TAG,"Client disconnected"); drop(i); }
        }
        bool fresh=w.packets_ok!=sent_ok;
        if(fresh || now_ms()-last_sent>=REPEAT_MS) {
            sent_ok=w.packets_ok;
            last_sent=now_ms();
            int n=format_line(line,sizeof(line),&w);
            if(n>0 && (size_t)n<sizeof(line)) for(size_t i=0;i<RF_WEATHER_FEED_MAX_CLIENTS;i++) send_to(i,line,n);
        }
    }
}
#endif
esp_err_t rf_weather_feed_start(void) {
#if CONFIG_RF_WEATHER_FEED_ENABLE
    port=CONFIG_RF_WEATHER_FEED_PORT;
    if(xTaskCreate(feed_task,"wxfeed",4096,NULL,3,NULL)!=pdPASS) { port=0; return ESP_ERR_NO_MEM; }
#endif
    return ESP_OK;
}
int rf_weather_feed_port(void) { return port; }
size_t rf_weather_feed_clients(void) { return client_count; }
