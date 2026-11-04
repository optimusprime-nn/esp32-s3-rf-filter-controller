#include "fan_math.h"
uint32_t rf_fan_duty(uint8_t percent, unsigned resolution_bits, bool inverted) {
    if(percent>100) percent=100;
    uint32_t full=1u<<resolution_bits;
    uint32_t duty=(full*percent+50)/100;
    return inverted?full-duty:duty;
}
uint32_t rf_fan_rpm(uint32_t pulses, uint32_t window_ms, unsigned pulses_per_rev) {
    if(!window_ms || !pulses_per_rev) return 0;
    return (uint32_t)(((uint64_t)pulses*60000u)/((uint64_t)window_ms*pulses_per_rev));
}
