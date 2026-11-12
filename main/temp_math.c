#include "temp_math.h"
#include <stdlib.h>
int32_t rf_lm75_millicelsius(uint8_t msb, uint8_t lsb) {
    int16_t raw=(int16_t)(((uint16_t)msb<<8)|lsb);
    return (int32_t)raw*1000/256;
}
uint8_t rf_aht_crc8(const uint8_t *d, size_t n) {
    uint8_t c=0xFF;
    for(size_t i=0;i<n;i++) {
        c^=d[i];
        for(int b=0;b<8;b++) c=(uint8_t)((c&0x80)?(c<<1)^0x31:(c<<1));
    }
    return c;
}
bool rf_aht_decode(const uint8_t f[7], int32_t *millicelsius, int32_t *millipercent_rh) {
    if((f[0]&0x80) || rf_aht_crc8(f,6)!=f[6]) return false;
    uint32_t h=((uint32_t)f[1]<<12)|((uint32_t)f[2]<<4)|(f[3]>>4);
    uint32_t t=((uint32_t)(f[3]&0x0F)<<16)|((uint32_t)f[4]<<8)|f[5];
    *millipercent_rh=(int32_t)(((int64_t)h*100000)>>20);
    *millicelsius=(int32_t)((((int64_t)t*200000)>>20)-50000);
    return true;
}
size_t rf_parse_i2c_addresses(const char *s, uint8_t *out, size_t max) {
    size_t n=0;
    if(!s) return 0;
    while(*s) {
        while(*s==' ' || *s==',') s++;
        if(!*s) break;
        char *end;
        long a=strtol(s,&end,0);
        /* 0x08 to 0x77 excludes the reserved I2C addresses. */
        if(end==s || a<0x08 || a>0x77 || n==max) return 0;
        out[n++]=(uint8_t)a;
        s=end;
        if(*s && *s!=',' && *s!=' ') return 0;
    }
    return n;
}
