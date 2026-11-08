#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
/* Reboot and power control for the SDR board (Radxa Zero 3).
   Soft reset: an active-low pulse on an open-drain line to a Radxa GPIO, which asks Linux
   to reboot cleanly. Hard reset: energises a relay whose normally-closed contact carries
   the Radxa's power, so the Radxa stays powered whenever this output is idle. */
typedef enum { RF_SDR_IDLE, RF_SDR_SOFT_RESET, RF_SDR_POWER_OFF } rf_sdr_state_t;
typedef struct {
    rf_sdr_state_t state;
    uint32_t soft_resets, hard_resets; /* since this controller booted */
    int64_t last_action_ms;            /* uptime of the last request, -1 for none */
    bool last_was_hard;
} rf_sdr_status_t;
/* Sets both outputs idle. Call first thing in app_main. */
esp_err_t rf_sdr_start(void);
/* ESP_ERR_INVALID_STATE while another reset is running or within the cooldown after one. */
esp_err_t rf_sdr_soft_reset(void);
esp_err_t rf_sdr_hard_reset(void);
void rf_sdr_status(rf_sdr_status_t *out);
const char *rf_sdr_state_name(rf_sdr_state_t s);
