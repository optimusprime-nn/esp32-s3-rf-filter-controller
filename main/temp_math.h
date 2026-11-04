#pragma once
#include <stddef.h>
#include <stdint.h>
/* LM75-family temperature register (LM75, TMP75, TMP102, PCT2075): two's complement, MSB first, 1/256 °C per bit. */
int32_t rf_lm75_millicelsius(uint8_t msb, uint8_t lsb);
/* Parses "0x48,0x49" into 7-bit addresses. Returns the count, or 0 if any entry is invalid or there are more than max. */
size_t rf_parse_i2c_addresses(const char *s, uint8_t *out, size_t max);
