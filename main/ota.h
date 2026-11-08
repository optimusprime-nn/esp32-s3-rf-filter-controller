#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
/* Firmware updates over HTTPS, ported from esp32p4_nts_time_server's ota_manager.
   A new image boots pending verification and is confirmed after a soak period with
   the management server up; until then a crash or reset makes the bootloader roll back. */
typedef struct {
    bool update_in_progress, pending_verify;
    uint32_t attempts, successes, failures, confirmations, rollbacks_detected;
    size_t bytes_written;
    char running_partition[17], boot_partition[17], next_partition[17];
    char running_version[33], previous_version[33], running_project[33];
    char last_result[40];
} rf_ota_status_t;
esp_err_t rf_ota_init(void);
/* Checks that the first bytes of an upload are an app image built for this project. */
esp_err_t rf_ota_check_header(const void *data, size_t length);
esp_err_t rf_ota_begin(size_t image_size);
esp_err_t rf_ota_write(const void *data, size_t length);
/* Verifies the whole image and makes it the next boot. */
esp_err_t rf_ota_finish(void);
void rf_ota_abort(void);
esp_err_t rf_ota_confirm(void);
/* Marks a still-pending running image invalid and reboots into the previous one.
   ESP_ERR_INVALID_STATE, doing nothing, when the running image isn't pending. */
esp_err_t rf_ota_reject(const char *reason);
/* The image a manual rollback would boot. ESP_ERR_NOT_FOUND when there's none. */
esp_err_t rf_ota_rollback_target(char *version, size_t version_len);
esp_err_t rf_ota_rollback(void);
void rf_ota_status(rf_ota_status_t *out);
/* Confirms the running image after CONFIG_RF_OTA_SOAK_SECONDS if web_ok() is then true,
   otherwise rolls it back. Does nothing for an image that is already confirmed. */
esp_err_t rf_ota_start_soak(bool (*web_ok)(void));
