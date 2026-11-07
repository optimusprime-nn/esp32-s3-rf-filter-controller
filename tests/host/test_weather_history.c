/* Host tests for the weather history ring. */
#include "weather_history.h"
#include <stdio.h>
static int failures;
#define CHECK(x) do { if(!(x)) { printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); failures++; } } while(0)
int main(void) {
    rf_weather_sample_t buf[4],out[8];
    rf_weather_history_t h;
    rf_vevor_reading_t r={.temp_dc=215,.humidity=40,.wind_raw=13,.gust_raw=3,.wind_dir_deg=266,.rain_raw=55,.uv_index=1,.light_lux=14457};
    uint32_t next;
    rf_weather_history_init(&h,buf,4);
    CHECK(rf_weather_history_read(&h,0,out,8,&next)==0 && next==0);
    CHECK(rf_weather_history_add(&h,&r,-80,100));
    CHECK(!rf_weather_history_add(&h,&r,-80,120));      /* under a minute later */
    CHECK(!rf_weather_history_add(&h,&r,-80,159));
    CHECK(rf_weather_history_add(&h,&r,-200,160));      /* RSSI clamps */
    CHECK(rf_weather_history_read(&h,0,out,8,&next)==2 && next==2);
    CHECK(out[0].t_s==100 && out[0].temp_dc==215 && out[0].rssi_dbm==-80 && out[0].light_lux==14457);
    CHECK(out[1].t_s==160 && out[1].rssi_dbm==-128);
    for(uint32_t t=220;t<=400;t+=60) { r.temp_dc=(int16_t)t; CHECK(rf_weather_history_add(&h,&r,-70,t)); }
    /* Six added into four slots: the two oldest are gone. */
    CHECK(rf_weather_history_oldest(&h)==2);
    CHECK(rf_weather_history_read(&h,0,out,8,&next)==4 && next==6);
    CHECK(out[0].t_s==220 && out[3].t_s==400 && out[3].temp_dc==400);
    /* Reading in pieces. */
    CHECK(rf_weather_history_read(&h,2,out,3,&next)==3 && next==5);
    CHECK(rf_weather_history_read(&h,next,out,3,&next)==1 && next==6 && out[0].t_s==400);
    CHECK(rf_weather_history_read(&h,next,out,3,&next)==0 && next==6);
    rf_weather_history_t empty;
    rf_weather_history_init(&empty,NULL,0);
    CHECK(!rf_weather_history_add(&empty,&r,-70,0));
    CHECK(rf_weather_history_read(&empty,0,out,8,&next)==0);
    if(failures) return 1;
    puts("weather history host tests PASSED");
    return 0;
}
