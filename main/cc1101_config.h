#pragma once
#include <stdbool.h>
#include <stdint.h>

/* CC1101 configuration registers 0x00 to 0x2E. */
#define RF_CC1101_CONFIG_REGS 0x2F
#define RF_CC1101_XTAL_HZ 26000000u

enum {
    RF_CC1101_IOCFG0=0x02, RF_CC1101_SYNC1=0x04, RF_CC1101_SYNC0=0x05, RF_CC1101_PKTLEN=0x06,
    RF_CC1101_PKTCTRL1=0x07, RF_CC1101_PKTCTRL0=0x08, RF_CC1101_FREQ2=0x0D, RF_CC1101_FREQ1=0x0E,
    RF_CC1101_FREQ0=0x0F, RF_CC1101_MDMCFG4=0x10, RF_CC1101_MDMCFG3=0x11, RF_CC1101_MDMCFG2=0x12,
    RF_CC1101_DEVIATN=0x15, RF_CC1101_MCSM1=0x17,
};

/* FREQ2..0 value for a carrier in kHz with a 26 MHz crystal. */
uint32_t rf_cc1101_freq_word(uint32_t khz);
/* Fills every configuration register for receiving the VEVOR 7-in-1: 2-FSK, 11.26 kBaud,
   38 kHz deviation, 203 kHz receive bandwidth, 16-bit sync 0xCA54, fixed 21-byte packets
   with RSSI and LQI appended. With invert the sync word is complemented, for a receiver
   whose FSK sense is opposite to the transmitter's; the payload must then be inverted too. */
void rf_cc1101_weather_config(uint32_t khz, bool invert, uint8_t regs[RF_CC1101_CONFIG_REGS]);
