#include "sdr_power.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"
static const char *TAG="sdr";
#define SOFT_GPIO ((gpio_num_t)CONFIG_RF_SDR_SOFT_RESET_GPIO)
#define RELAY_GPIO ((gpio_num_t)CONFIG_RF_SDR_RELAY_GPIO)
static portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
static rf_sdr_state_t state=RF_SDR_IDLE;
static uint32_t soft_count,hard_count;
static int64_t last_us=-1;
static int64_t idle_since_us=-1; /* when the last reset ended */
static bool last_hard;
static esp_timer_handle_t timer;
static void release(void *arg) {
    /* Open-drain high is released: the Radxa's pull-up holds the line inactive. */
    gpio_set_level(SOFT_GPIO,1);
    gpio_set_level(RELAY_GPIO,0);
    portENTER_CRITICAL(&mux);
    rf_sdr_state_t was=state;
    state=RF_SDR_IDLE;
    idle_since_us=esp_timer_get_time();
    portEXIT_CRITICAL(&mux);
    ESP_LOGI(TAG,"%s",was==RF_SDR_POWER_OFF?"SDR power restored":"SDR reboot request released");
}
esp_err_t rf_sdr_start(void) {
    /* Released before switching to output, so neither line glitches active. */
    gpio_set_level(SOFT_GPIO,1);
    gpio_set_level(RELAY_GPIO,0);
    /* Open drain never drives the line high, so it can't back-power the Radxa while its power is cut. */
    gpio_config_t soft={.pin_bit_mask=1ULL<<SOFT_GPIO,.mode=GPIO_MODE_OUTPUT_OD,
        .pull_up_en=GPIO_PULLUP_DISABLE,.pull_down_en=GPIO_PULLDOWN_DISABLE,.intr_type=GPIO_INTR_DISABLE};
    gpio_config_t relay={.pin_bit_mask=1ULL<<RELAY_GPIO,.mode=GPIO_MODE_OUTPUT,
        .pull_up_en=GPIO_PULLUP_DISABLE,.pull_down_en=GPIO_PULLDOWN_ENABLE,.intr_type=GPIO_INTR_DISABLE};
    esp_err_t e=gpio_config(&soft);
    if(e==ESP_OK) e=gpio_config(&relay);
    const esp_timer_create_args_t args={.callback=release,.name="sdr_reset"};
    if(e==ESP_OK && !timer) e=esp_timer_create(&args,&timer);
    if(e==ESP_OK) ESP_LOGI(TAG,"SDR soft reset on GPIO%d (active low), power relay on GPIO%d",(int)SOFT_GPIO,(int)RELAY_GPIO);
    return e;
}
static esp_err_t begin(rf_sdr_state_t next, uint32_t ms) {
    if(!timer) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&mux);
    /* The cooldown gives the Radxa time to start booting before another reset can interrupt it. */
    bool busy=state!=RF_SDR_IDLE ||
        (idle_since_us>=0 && esp_timer_get_time()-idle_since_us<(int64_t)CONFIG_RF_SDR_COOLDOWN_MS*1000);
    if(!busy) {
        state=next;
        last_us=esp_timer_get_time();
        last_hard=next==RF_SDR_POWER_OFF;
        if(last_hard) hard_count++; else soft_count++;
    }
    portEXIT_CRITICAL(&mux);
    if(busy) return ESP_ERR_INVALID_STATE;
    if(next==RF_SDR_POWER_OFF) gpio_set_level(RELAY_GPIO,1); else gpio_set_level(SOFT_GPIO,0);
    ESP_LOGW(TAG,"%s for %lu ms",next==RF_SDR_POWER_OFF?"SDR power cut":"SDR reboot requested",(unsigned long)ms);
    esp_err_t e=esp_timer_start_once(timer,(uint64_t)ms*1000);
    if(e!=ESP_OK) release(NULL);
    return e;
}
esp_err_t rf_sdr_soft_reset(void) { return begin(RF_SDR_SOFT_RESET,CONFIG_RF_SDR_SOFT_RESET_PULSE_MS); }
esp_err_t rf_sdr_hard_reset(void) { return begin(RF_SDR_POWER_OFF,CONFIG_RF_SDR_POWER_OFF_MS); }
void rf_sdr_status(rf_sdr_status_t *out) {
    portENTER_CRITICAL(&mux);
    out->state=state;
    out->soft_resets=soft_count;
    out->hard_resets=hard_count;
    out->last_action_ms=last_us<0?-1:last_us/1000;
    out->last_was_hard=last_hard;
    portEXIT_CRITICAL(&mux);
}
const char *rf_sdr_state_name(rf_sdr_state_t s) {
    return s==RF_SDR_SOFT_RESET?"reboot_requested":s==RF_SDR_POWER_OFF?"power_off":"idle";
}
