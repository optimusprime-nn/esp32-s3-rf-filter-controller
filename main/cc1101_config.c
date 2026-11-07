#include "cc1101_config.h"
#include <string.h>

uint32_t rf_cc1101_freq_word(uint32_t khz) {
    uint64_t hz=(uint64_t)khz*1000u;
    return (uint32_t)((hz*65536u+RF_CC1101_XTAL_HZ/2)/RF_CC1101_XTAL_HZ);
}
void rf_cc1101_weather_config(uint32_t khz, bool invert, uint8_t regs[RF_CC1101_CONFIG_REGS]) {
    /* Values follow TI SmartRF Studio for a 2-FSK receiver; the data rate, deviation and
       bandwidth registers are worked out for this station. */
    static const uint8_t base[RF_CC1101_CONFIG_REGS]={
        [0x00]=0x2E,          /* IOCFG2: high impedance, unused */
        [0x01]=0x2E,          /* IOCFG1 */
        [0x02]=0x06,          /* IOCFG0: high from sync word to end of packet */
        [0x03]=0x47,          /* FIFOTHR */
        [0x07]=0x24,          /* PKTCTRL1: PQT 1, append RSSI and LQI */
        [0x08]=0x00,          /* PKTCTRL0: fixed length, no CRC, no whitening */
        [0x0B]=0x06,          /* FSCTRL1: IF 152 kHz */
        [0x10]=0x88,          /* MDMCFG4: RX bandwidth 203 kHz, DRATE_E 8 */
        [0x11]=0xC6,          /* MDMCFG3: DRATE_M 198 -> 11.257 kBaud */
        [0x12]=0x02,          /* MDMCFG2: 2-FSK, 16 of 16 sync bits */
        [0x13]=0x22,          /* MDMCFG1 */
        [0x14]=0xF8,          /* MDMCFG0 */
        [0x15]=0x44,          /* DEVIATN: 38.1 kHz */
        [0x16]=0x07,          /* MCSM2 */
        [0x17]=0x30,          /* MCSM1: idle after a packet */
        [0x18]=0x18,          /* MCSM0: calibrate when leaving idle */
        [0x19]=0x16,          /* FOCCFG: track up to a quarter of the bandwidth */
        [0x1A]=0x6C,          /* BSCFG */
        [0x1B]=0x43,          /* AGCCTRL2 */
        [0x1C]=0x40,          /* AGCCTRL1 */
        [0x1D]=0x91,          /* AGCCTRL0 */
        [0x1E]=0x87, [0x1F]=0x6B, [0x20]=0xFB,
        [0x21]=0x56,          /* FREND1 */
        [0x22]=0x10,          /* FREND0 */
        [0x23]=0xE9, [0x24]=0x2A, [0x25]=0x00, [0x26]=0x1F,
        [0x27]=0x41, [0x28]=0x00, [0x29]=0x59, [0x2A]=0x7F, [0x2B]=0x3F,
        [0x2C]=0x81, [0x2D]=0x35, [0x2E]=0x09,
    };
    memcpy(regs,base,RF_CC1101_CONFIG_REGS);
    uint16_t sync=invert?(uint16_t)~0xCA54u:0xCA54u;
    regs[RF_CC1101_SYNC1]=(uint8_t)(sync>>8);
    regs[RF_CC1101_SYNC0]=(uint8_t)sync;
    regs[RF_CC1101_PKTLEN]=21;
    uint32_t f=rf_cc1101_freq_word(khz);
    regs[RF_CC1101_FREQ2]=(uint8_t)(f>>16);
    regs[RF_CC1101_FREQ1]=(uint8_t)(f>>8);
    regs[RF_CC1101_FREQ0]=(uint8_t)f;
}
