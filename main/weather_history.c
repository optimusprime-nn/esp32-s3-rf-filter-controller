#include "weather_history.h"
#include <string.h>

void rf_weather_history_init(rf_weather_history_t *h, rf_weather_sample_t *buf, size_t cap) {
    *h=(rf_weather_history_t){.buf=buf,.cap=cap};
}
bool rf_weather_history_add(rf_weather_history_t *h, const rf_vevor_reading_t *r, int rssi_dbm, uint32_t t_s) {
    if(!h->cap) return false;
    if(h->any && t_s-h->last_t_s<RF_WEATHER_HISTORY_INTERVAL_S) return false;
    rf_weather_sample_t *s=&h->buf[h->next_seq%h->cap];
    *s=(rf_weather_sample_t){.t_s=t_s,.temp_dc=r->temp_dc,.wind_raw=r->wind_raw,.wind_dir_deg=r->wind_dir_deg,
        .rain_raw=r->rain_raw,.light_lux=r->light_lux,.humidity=r->humidity,.gust_raw=r->gust_raw,
        .uv_index=r->uv_index,.rssi_dbm=(int8_t)(rssi_dbm<-128?-128:rssi_dbm>127?127:rssi_dbm)};
    h->next_seq++;
    h->any=true;
    h->last_t_s=t_s;
    return true;
}
uint32_t rf_weather_history_oldest(const rf_weather_history_t *h) {
    return h->next_seq>h->cap?h->next_seq-(uint32_t)h->cap:0;
}
size_t rf_weather_history_read(const rf_weather_history_t *h, uint32_t seq, rf_weather_sample_t *out, size_t max, uint32_t *next) {
    uint32_t oldest=rf_weather_history_oldest(h);
    if(seq<oldest) seq=oldest;
    size_t n=0;
    while(n<max && seq<h->next_seq) { out[n++]=h->buf[seq%h->cap]; seq++; }
    *next=seq;
    return n;
}
