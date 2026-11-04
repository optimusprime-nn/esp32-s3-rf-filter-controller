#include "fan.h"
#include "fan_math.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/pulse_cnt.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include <stdbool.h>
static const char *TAG="fan";
#define FAN_MODE LEDC_LOW_SPEED_MODE
#define FAN_TIMER LEDC_TIMER_0
#define FAN_CHANNEL LEDC_CHANNEL_0
#define FAN_BITS 10
#define TACH_WINDOW_MS 1000
static SemaphoreHandle_t lock;
static volatile uint8_t percent;
static volatile int32_t rpm=-1;
static pcnt_unit_handle_t tach;
static esp_timer_handle_t tach_timer;
static bool stall_logged;
static uint8_t load_percent(void) {
    nvs_handle_t h;
    uint8_t p=CONFIG_RF_FAN_DEFAULT_PERCENT;
    if(nvs_open("rfconfig",NVS_READONLY,&h)==ESP_OK) {
        if(nvs_get_u8(h,"fan",&p)!=ESP_OK || p>100) p=CONFIG_RF_FAN_DEFAULT_PERCENT;
        nvs_close(h);
    }
    return p;
}
static esp_err_t save_percent(uint8_t p) {
    nvs_handle_t h;
    esp_err_t e=nvs_open("rfconfig",NVS_READWRITE,&h);
    if(e!=ESP_OK) return e;
    e=nvs_set_u8(h,"fan",p);
    if(e==ESP_OK) e=nvs_commit(h);
    nvs_close(h);
    return e;
}
static esp_err_t apply(uint8_t p) {
    esp_err_t e=ledc_set_duty(FAN_MODE,FAN_CHANNEL,rf_fan_duty(p,FAN_BITS,CONFIG_RF_FAN_PWM_INVERTED));
    if(e==ESP_OK) e=ledc_update_duty(FAN_MODE,FAN_CHANNEL);
    if(e==ESP_OK) { percent=p; stall_logged=false; }
    return e;
}
static void tach_sample(void *arg) {
    int count=0;
    if(pcnt_unit_get_count(tach,&count)!=ESP_OK) return;
    pcnt_unit_clear_count(tach);
    rpm=(int32_t)rf_fan_rpm(count<0?0:(uint32_t)count,TACH_WINDOW_MS,CONFIG_RF_FAN_PULSES_PER_REV);
    if(!rpm && percent>=20 && !stall_logged) { ESP_LOGW(TAG,"Fan reports 0 RPM at %u%%",(unsigned)percent); stall_logged=true; }
}
static esp_err_t tach_start(void) {
    pcnt_unit_config_t unit={.low_limit=-1,.high_limit=32767};
    esp_err_t e=pcnt_new_unit(&unit,&tach);
    if(e!=ESP_OK) return e;
    pcnt_glitch_filter_config_t filter={.max_glitch_ns=1000};
    pcnt_chan_config_t chan_cfg={.edge_gpio_num=CONFIG_RF_FAN_TACH_GPIO,.level_gpio_num=-1};
    pcnt_channel_handle_t chan;
    e=pcnt_unit_set_glitch_filter(tach,&filter);
    if(e==ESP_OK) e=pcnt_new_channel(tach,&chan_cfg,&chan);
    if(e==ESP_OK) e=pcnt_channel_set_edge_action(chan,PCNT_CHANNEL_EDGE_ACTION_HOLD,PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    /* The tach output is open-collector; the internal pull-up is a fallback for a missing external one. */
    if(e==ESP_OK) e=gpio_pullup_en(CONFIG_RF_FAN_TACH_GPIO);
    if(e==ESP_OK) e=pcnt_unit_enable(tach);
    if(e==ESP_OK) e=pcnt_unit_clear_count(tach);
    if(e==ESP_OK) e=pcnt_unit_start(tach);
    const esp_timer_create_args_t timer={.callback=tach_sample,.name="fan_tach"};
    if(e==ESP_OK) e=esp_timer_create(&timer,&tach_timer);
    if(e==ESP_OK) e=esp_timer_start_periodic(tach_timer,TACH_WINDOW_MS*1000ULL);
    return e;
}
esp_err_t rf_fan_start(void) {
    if(lock) return ESP_ERR_INVALID_STATE;
    lock=xSemaphoreCreateMutex();
    if(!lock) return ESP_ERR_NO_MEM;
    uint8_t p=load_percent();
    ledc_timer_config_t timer={.speed_mode=FAN_MODE,.duty_resolution=FAN_BITS,.timer_num=FAN_TIMER,
        .freq_hz=CONFIG_RF_FAN_PWM_FREQ_HZ,.clk_cfg=LEDC_AUTO_CLK};
    esp_err_t e=ledc_timer_config(&timer);
    ledc_channel_config_t chan={.gpio_num=CONFIG_RF_FAN_PWM_GPIO,.speed_mode=FAN_MODE,.channel=FAN_CHANNEL,
        .timer_sel=FAN_TIMER,.duty=rf_fan_duty(p,FAN_BITS,CONFIG_RF_FAN_PWM_INVERTED)};
    if(e==ESP_OK) e=ledc_channel_config(&chan);
    if(e!=ESP_OK) return e;
    percent=p;
    ESP_LOGI(TAG,"Fan PWM on GPIO%d at %u%%",CONFIG_RF_FAN_PWM_GPIO,(unsigned)p);
    if(CONFIG_RF_FAN_TACH_GPIO>=0) {
        e=tach_start();
        if(e!=ESP_OK) { rpm=-1; ESP_LOGW(TAG,"Tachometer unavailable: %s",esp_err_to_name(e)); }
    }
    return ESP_OK;
}
esp_err_t rf_fan_set(uint8_t p) {
    if(p>100) return ESP_ERR_INVALID_ARG;
    if(!lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(lock,portMAX_DELAY);
    esp_err_t e=apply(p);
    if(e==ESP_OK) e=save_percent(p);
    xSemaphoreGive(lock);
    return e;
}
uint8_t rf_fan_percent(void) {
    return percent;
}
int32_t rf_fan_rpm_now(void) {
    return tach_timer?rpm:-1;
}
