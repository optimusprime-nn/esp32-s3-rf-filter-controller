/* Host tests for LM75 and AHT20/AHT30 conversion and the I2C address list. */
#include "temp_math.h"
#include <stdio.h>
static int failures;
#define CHECK(x) do { if(!(x)) { printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); failures++; } } while(0)
int main(void) {
    uint8_t a[4];
    CHECK(rf_lm75_millicelsius(0x19,0x00)==25000);
    CHECK(rf_lm75_millicelsius(0x19,0x80)==25500);
    CHECK(rf_lm75_millicelsius(0x00,0x10)==62);
    CHECK(rf_lm75_millicelsius(0xFF,0x80)==-500);
    CHECK(rf_lm75_millicelsius(0xC9,0x00)==-55000);
    CHECK(rf_lm75_millicelsius(0x7D,0x00)==125000);
    CHECK(rf_parse_i2c_addresses("0x48",a,4)==1 && a[0]==0x48);
    CHECK(rf_parse_i2c_addresses("0x48,0x49, 0x4f",a,4)==3 && a[1]==0x49 && a[2]==0x4f);
    CHECK(rf_parse_i2c_addresses("",a,4)==0);
    CHECK(rf_parse_i2c_addresses("0x07",a,4)==0);
    CHECK(rf_parse_i2c_addresses("0x78",a,4)==0);
    CHECK(rf_parse_i2c_addresses("0x48,zz",a,4)==0);
    CHECK(rf_parse_i2c_addresses("0x48x",a,4)==0);
    CHECK(rf_parse_i2c_addresses("0x48,0x49,0x4a,0x4b,0x4c",a,4)==0);
    /* CRC-8 0x31/0xFF: the standard check value, and Sensirion's 0xBEEF example. */
    CHECK(rf_aht_crc8((const uint8_t *)"123456789",9)==0xF7);
    CHECK(rf_aht_crc8((const uint8_t[]){0xBE,0xEF},2)==0x92);
    {
        /* 50 % RH (0x80000) and 25 °C (0x60000). */
        uint8_t f[7]={0x1C,0x80,0x00,0x06,0x00,0x00,0};
        int32_t mc=0,mrh=0;
        f[6]=rf_aht_crc8(f,6);
        CHECK(rf_aht_decode(f,&mc,&mrh) && mc==25000 && mrh==50000);
        f[6]^=1;
        CHECK(!rf_aht_decode(f,&mc,&mrh));
        /* Busy: rejected even with a good CRC. */
        f[0]=0x9C; f[6]=rf_aht_crc8(f,6);
        CHECK(!rf_aht_decode(f,&mc,&mrh));
        /* Range ends: 0 % and -50 °C, then ~100 % and ~150 °C. */
        uint8_t lo[7]={0x1C,0,0,0,0,0,0}; lo[6]=rf_aht_crc8(lo,6);
        CHECK(rf_aht_decode(lo,&mc,&mrh) && mc==-50000 && mrh==0);
        uint8_t hi[7]={0x1C,0xFF,0xFF,0xFF,0xFF,0xFF,0}; hi[6]=rf_aht_crc8(hi,6);
        CHECK(rf_aht_decode(hi,&mc,&mrh) && mc==149999 && mrh==99999);
    }
    if(failures) return 1;
    puts("temperature host tests PASSED");
    return 0;
}
