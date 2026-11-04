/* Host tests for LM75 conversion and the I2C address list. */
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
    if(failures) return 1;
    puts("temperature host tests PASSED");
    return 0;
}
