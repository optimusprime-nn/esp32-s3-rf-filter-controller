#include "weather_decode.h"

/* Multi-byte fields carry an offset of one in every byte. */
static uint8_t unoffset(uint8_t v) { return (uint8_t)(v-1u); }

rf_vevor_result_t rf_vevor_decode(const uint8_t *b, size_t len, rf_vevor_reading_t *out) {
    if(len<RF_VEVOR_PACKET_LEN) return RF_VEVOR_SHORT;
    uint8_t sum=0;
    for(size_t i=0;i<19;i++) sum=(uint8_t)(sum+b[i]);
    if(sum!=b[19]) return RF_VEVOR_CHECKSUM;
    if(b[20]!=(uint8_t)(b[18]+1u)) return RF_VEVOR_COUNTER;
    if(b[0]!=0xAA || (b[1]&0xF0)!=0) return RF_VEVOR_NOT_WEATHER;
    rf_vevor_reading_t r={0};
    r.channel=b[1]&0x0F;
    r.id=(uint16_t)(b[2]<<8|b[3]);
    r.battery_low=(b[4]&0x80)!=0;
    r.temp_dc=(int16_t)(((b[5]<<8)|b[6])-500);
    r.humidity=b[7];
    r.wind_raw=(uint16_t)(unoffset(b[8])<<8|unoffset(b[9]));
    r.gust_raw=b[10];
    r.wind_dir_deg=(uint16_t)((unoffset(b[11])&0x0F)<<8|unoffset(b[12]));
    r.rain_raw=(uint16_t)(unoffset(b[13])<<8|unoffset(b[14]));
    int uv=(b[15]&0x1F)-1;
    r.uv_index=(int8_t)(uv<0?0:uv);
    uint16_t lux=(uint16_t)(unoffset(b[16])<<8|unoffset(b[17]));
    r.light_lux=(lux&0x8000)?(uint32_t)(lux&0x7FFF)*10u:lux;
    r.counter=b[18];
    *out=r;
    return RF_VEVOR_OK;
}
double rf_vevor_wind_kmh(uint16_t raw) { return raw/8.333; }
double rf_vevor_gust_kmh(uint8_t raw) { return raw/1.25; }
double rf_vevor_rain_mm(uint16_t raw) { return raw*0.233; }
int rf_cc1101_rssi_dbm(uint8_t raw) {
    int v=raw>=128?(int)raw-256:raw;
    return v/2-74;
}
