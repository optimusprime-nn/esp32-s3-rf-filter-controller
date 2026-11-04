#include "temp_math.h"
#include <stdlib.h>
int32_t rf_lm75_millicelsius(uint8_t msb, uint8_t lsb) {
    int16_t raw=(int16_t)(((uint16_t)msb<<8)|lsb);
    return (int32_t)raw*1000/256;
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
