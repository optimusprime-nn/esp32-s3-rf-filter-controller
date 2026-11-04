#pragma once
#include <stdbool.h>
#include <stdint.h>
/* LEDC duty for a fan speed, so that 100% always means the fan's PWM input is held high. */
uint32_t rf_fan_duty(uint8_t percent, unsigned resolution_bits, bool inverted);
uint32_t rf_fan_rpm(uint32_t pulses, uint32_t window_ms, unsigned pulses_per_rev);
