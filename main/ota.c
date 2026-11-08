#include "ota.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"
static const char *TAG="OTA";
static const char *NS="ota_hist";
static SemaphoreHandle_t lock;
static esp_ota_handle_t handle;
static const esp_partition_t *target;
static rf_ota_status_t st;
static bool (*soak_web_ok)(void);

static void label(char *dst, size_t n, const esp_partition_t *p) { snprintf(dst,n,"%s",p?p->label:""); }
static void result(const char *r) { snprintf(st.last_result,sizeof(st.last_result),"%s",r); }

/* pending holds the slot an upload or rollback staged, so the next boot can tell whether
   the bootloader actually ran it. */
static void load(char pending[17]) {
    pending[0]='\0';
    nvs_handle_t h;
    if(nvs_open(NS,NVS_READONLY,&h)!=ESP_OK) return;
    nvs_get_u32(h,"attempts",&st.attempts);
    nvs_get_u32(h,"successes",&st.successes);
    nvs_get_u32(h,"failures",&st.failures);
    nvs_get_u32(h,"confirms",&st.confirmations);
    nvs_get_u32(h,"rollbacks",&st.rollbacks_detected);
    size_t n=17;
    if(nvs_get_str(h,"pending",pending,&n)!=ESP_OK) pending[0]='\0';
    n=sizeof(st.previous_version);
    if(nvs_get_str(h,"prevver",st.previous_version,&n)!=ESP_OK) st.previous_version[0]='\0';
    nvs_close(h);
}
static void save(const char *pending) {
    nvs_handle_t h;
    if(nvs_open(NS,NVS_READWRITE,&h)!=ESP_OK) return;
    nvs_set_u32(h,"attempts",st.attempts);
    nvs_set_u32(h,"successes",st.successes);
    nvs_set_u32(h,"failures",st.failures);
    nvs_set_u32(h,"confirms",st.confirmations);
    nvs_set_u32(h,"rollbacks",st.rollbacks_detected);
    if(st.previous_version[0]) nvs_set_str(h,"prevver",st.previous_version);
    if(pending && *pending) nvs_set_str(h,"pending",pending); else nvs_erase_key(h,"pending");
    nvs_commit(h);
    nvs_close(h);
}
esp_err_t rf_ota_init(void) {
    if(!lock) lock=xSemaphoreCreateMutex();
    if(!lock) return ESP_ERR_NO_MEM;
    char pending[17];
    load(pending);
    const esp_partition_t *running=esp_ota_get_running_partition();
    label(st.running_partition,sizeof(st.running_partition),running);
    label(st.boot_partition,sizeof(st.boot_partition),esp_ota_get_boot_partition());
    label(st.next_partition,sizeof(st.next_partition),esp_ota_get_next_update_partition(NULL));
    esp_app_desc_t d;
    if(running && esp_ota_get_partition_description(running,&d)==ESP_OK) {
        snprintf(st.running_version,sizeof(st.running_version),"%s",d.version);
        snprintf(st.running_project,sizeof(st.running_project),"%s",d.project_name);
    }
    esp_ota_img_states_t s;
    st.pending_verify=running && esp_ota_get_state_partition(running,&s)==ESP_OK && s==ESP_OTA_IMG_PENDING_VERIFY;
    if(pending[0] && strcmp(pending,st.running_partition)) {
        st.rollbacks_detected++;
        st.failures++;
        result("rollback_detected");
        save(NULL);
        ESP_LOGW(TAG,"OTA rollback detected: staged=%s running=%s",pending,st.running_partition);
    } else result(st.pending_verify?"pending_verification":"ready");
    ESP_LOGI(TAG,"running=%s (%s) boot=%s next=%s state=%s",st.running_partition,st.running_version,
        st.boot_partition,st.next_partition,st.pending_verify?"pending_verify":"valid");
    return ESP_OK;
}
esp_err_t rf_ota_check_header(const void *data, size_t length) {
    const size_t off=sizeof(esp_image_header_t)+sizeof(esp_image_segment_header_t);
    if(length<off+sizeof(esp_app_desc_t)) return ESP_ERR_INVALID_SIZE;
    const esp_image_header_t *h=data;
    esp_app_desc_t d;
    memcpy(&d,(const uint8_t*)data+off,sizeof(d));
    if(h->magic!=ESP_IMAGE_HEADER_MAGIC || d.magic_word!=ESP_APP_DESC_MAGIC_WORD) return ESP_ERR_INVALID_VERSION;
    if(h->chip_id!=CONFIG_IDF_FIRMWARE_CHIP_ID) return ESP_ERR_INVALID_VERSION;
    if(strncmp(d.project_name,esp_app_get_description()->project_name,sizeof(d.project_name))) {
        ESP_LOGW(TAG,"Refusing an image for project %.32s",d.project_name);
        return ESP_ERR_INVALID_VERSION;
    }
    return ESP_OK;
}
esp_err_t rf_ota_begin(size_t image_size) {
    if(!lock) return ESP_ERR_INVALID_STATE;
    if(xSemaphoreTake(lock,pdMS_TO_TICKS(1000))!=pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t e=ESP_OK;
    if(st.update_in_progress) e=ESP_ERR_INVALID_STATE;
    else {
        target=esp_ota_get_next_update_partition(NULL);
        if(!target) { result("no_update_partition"); e=ESP_ERR_NOT_FOUND; }
        else if(!image_size || image_size>target->size) { result("invalid_image_size"); e=ESP_ERR_INVALID_SIZE; }
        else {
            e=esp_ota_begin(target,image_size,&handle);
            st.attempts++;
            if(e==ESP_OK) { st.update_in_progress=true; st.bytes_written=0; result("receiving"); }
            else { st.failures++; result("begin_failed"); }
            save(NULL);
        }
    }
    xSemaphoreGive(lock);
    return e;
}
esp_err_t rf_ota_write(const void *data, size_t length) {
    if(!st.update_in_progress) return ESP_ERR_INVALID_STATE;
    esp_err_t e=esp_ota_write(handle,data,length);
    if(e==ESP_OK) st.bytes_written+=length;
    return e;
}
void rf_ota_abort(void) {
    if(!st.update_in_progress) return;
    esp_ota_abort(handle);
    st.update_in_progress=false;
    st.failures++;
    result("aborted");
    save(NULL);
    ESP_LOGW(TAG,"OTA upload aborted after %u bytes",(unsigned)st.bytes_written);
}
esp_err_t rf_ota_finish(void) {
    if(!st.update_in_progress || !target) return ESP_ERR_INVALID_STATE;
    /* esp_ota_end checks the image's structure and SHA-256. */
    esp_err_t e=esp_ota_end(handle);
    if(e==ESP_OK) e=esp_ota_set_boot_partition(target);
    st.update_in_progress=false;
    if(e==ESP_OK) {
        st.successes++;
        snprintf(st.previous_version,sizeof(st.previous_version),"%s",st.running_version);
        label(st.boot_partition,sizeof(st.boot_partition),target);
        result("staged_reboot_required");
        save(target->label);
        ESP_LOGI(TAG,"OTA image staged on %s (%u bytes); reboot required",target->label,(unsigned)st.bytes_written);
    } else {
        st.failures++;
        result("validation_or_stage_failed");
        save(NULL);
        ESP_LOGE(TAG,"OTA image rejected: %s",esp_err_to_name(e));
    }
    target=NULL;
    return e;
}
esp_err_t rf_ota_confirm(void) {
    const esp_partition_t *running=esp_ota_get_running_partition();
    esp_ota_img_states_t s;
    esp_err_t e=running?esp_ota_get_state_partition(running,&s):ESP_ERR_NOT_FOUND;
    if(e!=ESP_OK) return e;
    if(s!=ESP_OTA_IMG_PENDING_VERIFY) { st.pending_verify=false; save(NULL); return ESP_OK; }
    e=esp_ota_mark_app_valid_cancel_rollback();
    if(e==ESP_OK) {
        st.pending_verify=false;
        st.confirmations++;
        result("running_image_confirmed");
        save(NULL);
        ESP_LOGI(TAG,"Running OTA image confirmed valid");
    }
    return e;
}
esp_err_t rf_ota_reject(const char *reason) {
    const esp_partition_t *running=esp_ota_get_running_partition();
    esp_ota_img_states_t s;
    if(!running || esp_ota_get_state_partition(running,&s)!=ESP_OK || s!=ESP_OTA_IMG_PENDING_VERIFY) return ESP_ERR_INVALID_STATE;
    ESP_LOGE(TAG,"Rejecting pending OTA image: %s",reason);
    result("running_image_rejected");
    return esp_ota_mark_app_invalid_rollback_and_reboot();
}
/* The other OTA slot, if it holds a usable image and nothing is already staged. */
static const esp_partition_t *other_slot(void) {
    const esp_partition_t *running=esp_ota_get_running_partition();
    if(!running || running->subtype<ESP_PARTITION_SUBTYPE_APP_OTA_MIN || running->subtype>=ESP_PARTITION_SUBTYPE_APP_OTA_MAX) return NULL;
    if(esp_ota_get_boot_partition()!=running) return NULL;
    const esp_partition_t *other=esp_ota_get_next_update_partition(NULL);
    if(!other || other==running) return NULL;
    esp_ota_img_states_t s;
    if(esp_ota_get_state_partition(other,&s)==ESP_OK && (s==ESP_OTA_IMG_INVALID || s==ESP_OTA_IMG_ABORTED)) return NULL;
    return other;
}
esp_err_t rf_ota_rollback_target(char *version, size_t version_len) {
    if(version && version_len) version[0]='\0';
    const esp_partition_t *t=other_slot();
    esp_app_desc_t d;
    if(!t || esp_ota_get_partition_description(t,&d)!=ESP_OK) return ESP_ERR_NOT_FOUND;
    if(version && version_len) snprintf(version,version_len,"%s",d.version);
    return ESP_OK;
}
esp_err_t rf_ota_rollback(void) {
    if(!lock) return ESP_ERR_INVALID_STATE;
    if(xSemaphoreTake(lock,pdMS_TO_TICKS(1000))!=pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t e;
    const esp_partition_t *t=st.update_in_progress?NULL:other_slot();
    esp_app_desc_t d;
    if(!t || esp_ota_get_partition_description(t,&d)!=ESP_OK) { result("rollback_unavailable"); e=ESP_ERR_NOT_FOUND; }
    else {
        /* Verifies the whole image; a corrupt slot is refused and the boot choice is unchanged. */
        e=esp_ota_set_boot_partition(t);
        if(e==ESP_OK) {
            snprintf(st.previous_version,sizeof(st.previous_version),"%s",st.running_version);
            label(st.boot_partition,sizeof(st.boot_partition),t);
            result("manual_rollback_staged");
            save(t->label);
            ESP_LOGW(TAG,"Manual rollback: %s -> %s (%s); reboot required",st.running_partition,t->label,d.version);
        } else {
            result("rollback_validation_failed");
            ESP_LOGE(TAG,"Manual rollback to %s refused: %s",t->label,esp_err_to_name(e));
        }
    }
    xSemaphoreGive(lock);
    return e;
}
void rf_ota_status(rf_ota_status_t *out) { *out=st; }
static void soak_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(CONFIG_RF_OTA_SOAK_SECONDS*1000ULL));
    /* HTTPS is the only way to replace firmware remotely, so an image that can't
       serve it goes back to the previous one. */
    if(soak_web_ok && soak_web_ok()) {
        esp_err_t e=rf_ota_confirm();
        if(e!=ESP_OK) ESP_LOGW(TAG,"OTA confirmation failed: %s",esp_err_to_name(e));
    } else rf_ota_reject("HTTPS management did not start");
    vTaskDelete(NULL);
}
esp_err_t rf_ota_start_soak(bool (*web_ok)(void)) {
    if(!st.pending_verify) return ESP_OK;
    soak_web_ok=web_ok;
    ESP_LOGW(TAG,"New firmware pending verification; confirming in %d s if HTTPS management is up",CONFIG_RF_OTA_SOAK_SECONDS);
    if(xTaskCreate(soak_task,"ota_soak",4096,NULL,3,NULL)!=pdPASS) return rf_ota_confirm();
    return ESP_OK;
}
