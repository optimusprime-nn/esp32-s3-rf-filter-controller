/* Host tests for fan duty and RPM arithmetic. */
#include "fan_math.h"
#include <stdio.h>
static int failures;
#define CHECK(x) do { if(!(x)) { printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); failures++; } } while(0)
int main(void) {
    CHECK(rf_fan_duty(0,10,false)==0);
    CHECK(rf_fan_duty(100,10,false)==1024);
    CHECK(rf_fan_duty(50,10,false)==512);
    CHECK(rf_fan_duty(33,10,false)==338);
    CHECK(rf_fan_duty(0,10,true)==1024);
    CHECK(rf_fan_duty(100,10,true)==0);
    CHECK(rf_fan_duty(25,10,true)==768);
    CHECK(rf_fan_duty(255,10,false)==1024);
    for(unsigned p=1;p<=100;p++) CHECK(rf_fan_duty((unsigned char)p,10,false)>=rf_fan_duty((unsigned char)(p-1),10,false));
    CHECK(rf_fan_rpm(0,1000,2)==0);
    CHECK(rf_fan_rpm(40,1000,2)==1200);
    CHECK(rf_fan_rpm(100,500,2)==6000);
    CHECK(rf_fan_rpm(40,0,2)==0);
    CHECK(rf_fan_rpm(40,1000,0)==0);
    CHECK(rf_fan_rpm(1000,1000,2)==30000);
    CHECK(rf_fan_rpm(70000,1000,1)==4200000);
    if(failures) return 1;
    puts("fan host tests PASSED");
    return 0;
}
