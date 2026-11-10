#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* What each ESP32-S3 GPIO is for and how it is set up right now, for the GPIO page. */
#define RF_GPIO_INFO_MAX 49
typedef struct {
    uint8_t gpio;
    const char *function;   /* NULL when this firmware doesn't use the pin */
    const char *group;      /* rf, sdr, fan, i2c, weather, usb, flash or NULL */
    const char *note;       /* strapping, flash or PSRAM, UART0..., or NULL */
    char detail[64];        /* what the pin is doing, from its driver: "Filter 2 selected" */
    bool config_read;       /* false for flash/PSRAM pins, which aren't read */
    bool input, output, open_drain, pull_up, pull_down;
    bool peripheral;        /* driven by a peripheral (PWM, I2C, SPI, USB), not as a plain GPIO */
    uint8_t drive;          /* 0 to 3 */
    int8_t level;           /* 0 or 1, -1 when it can't be read without changing the pin */
} rf_gpio_info_t;
/* Fills out for every GPIO the ESP32-S3 has, in order. Returns how many. */
size_t rf_gpio_info(rf_gpio_info_t *out, size_t max);
