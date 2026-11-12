#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* LM75-family temperature register (LM75, TMP75, TMP102, PCT2075): two's complement, MSB first, 1/256 °C per bit. */
int32_t rf_lm75_millicelsius(uint8_t msb, uint8_t lsb);
/* CRC-8 of AHT20/AHT30 frames: polynomial 0x31, initial value 0xFF. */
uint8_t rf_aht_crc8(const uint8_t *d, size_t n);
/* AHT20/AHT30 measurement frame: status, 20-bit humidity, 20-bit temperature, CRC.
   False while the sensor is busy or when the CRC doesn't match. */
bool rf_aht_decode(const uint8_t f[7], int32_t *millicelsius, int32_t *millipercent_rh);
/* Parses "0x48,0x49" into 7-bit addresses. Returns the count, or 0 if any entry is invalid or there are more than max. */
size_t rf_parse_i2c_addresses(const char *s, uint8_t *out, size_t max);
