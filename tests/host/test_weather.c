/* Host tests for the VEVOR 7-in-1 decoder and the CC1101 receive configuration. */
#include "weather_decode.h"
#include "cc1101_config.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
static int failures;
#define CHECK(x) do { if(!(x)) { printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); failures++; } } while(0)
/* Sample frame from the rtl_433 vevor_7in1 decoder's documentation. */
static const uint8_t sample[RF_VEVOR_PACKET_LEN]={0xAA,0x00,0xf8,0xf7,0x9d,0x02,0xe3,0x32,0x01,0x0e,0x03,
    0x02,0x0b,0x01,0x38,0x02,0x39,0x7a,0x86,0xe0,0x87};
static void fix_checksum(uint8_t *b) {
    uint8_t s=0;
    for(int i=0;i<19;i++) s=(uint8_t)(s+b[i]);
    b[19]=s;
    b[20]=(uint8_t)(b[18]+1);
}
int main(void) {
    rf_vevor_reading_t r;
    CHECK(rf_vevor_decode(sample,sizeof(sample),&r)==RF_VEVOR_OK);
    CHECK(r.id==0xf8f7);
    CHECK(r.channel==0);
    CHECK(r.battery_low);
    CHECK(r.temp_dc==239);
    CHECK(r.humidity==50);
    CHECK(r.wind_raw==13);
    CHECK(fabs(rf_vevor_wind_kmh(r.wind_raw)-1.56)<0.01);
    CHECK(r.gust_raw==3 && fabs(rf_vevor_gust_kmh(r.gust_raw)-2.4)<0.001);
    CHECK(r.wind_dir_deg==266);
    CHECK(r.rain_raw==55 && fabs(rf_vevor_rain_mm(r.rain_raw)-12.815)<0.001);
    CHECK(r.uv_index==1);
    CHECK(r.light_lux==14457);
    CHECK(r.counter==0x86);

    uint8_t b[RF_VEVOR_PACKET_LEN];
    CHECK(rf_vevor_decode(sample,20,&r)==RF_VEVOR_SHORT);
    memcpy(b,sample,sizeof(b)); b[7]^=1;
    CHECK(rf_vevor_decode(b,sizeof(b),&r)==RF_VEVOR_CHECKSUM);
    memcpy(b,sample,sizeof(b)); b[20]=0x88;
    CHECK(rf_vevor_decode(b,sizeof(b),&r)==RF_VEVOR_COUNTER);
    memcpy(b,sample,sizeof(b)); b[18]=0xff; fix_checksum(b);
    CHECK(b[20]==0x00 && rf_vevor_decode(b,sizeof(b),&r)==RF_VEVOR_OK);
    memcpy(b,sample,sizeof(b)); b[0]=0xAB; fix_checksum(b);
    CHECK(rf_vevor_decode(b,sizeof(b),&r)==RF_VEVOR_NOT_WEATHER);
    /* Below zero, light above 32767 lux, UV byte of zero, channel bits. */
    memcpy(b,sample,sizeof(b));
    b[1]=0x03; b[4]=0x1d; b[5]=0x01; b[6]=0x90; b[15]=0x00; b[16]=0x90; b[17]=0x01; fix_checksum(b);
    CHECK(rf_vevor_decode(b,sizeof(b),&r)==RF_VEVOR_OK);
    CHECK(r.channel==3 && !r.battery_low);
    CHECK(r.temp_dc==-100);
    CHECK(r.uv_index==0);
    CHECK(r.light_lux==38400);
    /* Zero bytes wrap to 0xff after the offset is removed. */
    memcpy(b,sample,sizeof(b)); b[8]=0x01; b[9]=0x00; fix_checksum(b);
    CHECK(rf_vevor_decode(b,sizeof(b),&r)==RF_VEVOR_OK && r.wind_raw==0x00ff);

    CHECK(rf_cc1101_rssi_dbm(0)==-74);
    CHECK(rf_cc1101_rssi_dbm(0x80)==-138);
    CHECK(rf_cc1101_rssi_dbm(0xD0)==-98);
    CHECK(rf_cc1101_rssi_dbm(40)==-54);

    CHECK(rf_cc1101_freq_word(915000)==0x23313B);
    CHECK(rf_cc1101_freq_word(868300)==0x21656A);
    uint8_t regs[RF_CC1101_CONFIG_REGS];
    rf_cc1101_weather_config(915000,false,regs);
    CHECK(regs[RF_CC1101_FREQ2]==0x23 && regs[RF_CC1101_FREQ1]==0x31 && regs[RF_CC1101_FREQ0]==0x3B);
    CHECK(regs[RF_CC1101_SYNC1]==0xCA && regs[RF_CC1101_SYNC0]==0x54);
    CHECK(regs[RF_CC1101_PKTLEN]==RF_VEVOR_PACKET_LEN);
    CHECK(regs[RF_CC1101_MDMCFG4]==0x58 && regs[RF_CC1101_MDMCFG3]==0xC6 && regs[RF_CC1101_DEVIATN]==0x44);
    CHECK(regs[RF_CC1101_MDMCFG2]==0x02 && regs[RF_CC1101_PKTCTRL0]==0x00 && regs[RF_CC1101_IOCFG0]==0x06);
    rf_cc1101_weather_config(915000,true,regs);
    CHECK(regs[RF_CC1101_SYNC1]==0x35 && regs[RF_CC1101_SYNC0]==0xAB);
    if(failures) return 1;
    puts("weather host tests PASSED");
    return 0;
}
