#include "fan_control.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include <stdbool.h>

/* Provisional pins: verify against final ESP32-S3 module and PCB routing.
 * 4-wire fan PWM input MUST be driven via an external open-collector/open-drain
 * transistor stage, NOT directly by an ESP32-S3 push-pull GPIO.
 * LEDC output inversion compensates for the inverting NPN/NMOS sink stage:
 * 100% command = sink OFF, fan PWM pulled HIGH externally.
 * GPIO low during reset should also mean sink OFF with suitable driver topology.
 */
#define FAN1_PWM_GPIO 15
#define FAN2_PWM_GPIO 16
#define FAN_PWM_HZ 25000
#define FAN_TIMER LEDC_TIMER_0
static const char *TAG="fans";
static bool ready;
static uint8_t duty_percent[RF_FAN_COUNT]={100,100};
static const ledc_channel_t channels[RF_FAN_COUNT]={LEDC_CHANNEL_0,LEDC_CHANNEL_1};
static const int pins[RF_FAN_COUNT]={FAN1_PWM_GPIO,FAN2_PWM_GPIO};

esp_err_t rf_fans_init(void) {
    if(ready) return ESP_OK;
    ledc_timer_config_t timer={
        .speed_mode=LEDC_LOW_SPEED_MODE,
        .duty_resolution=LEDC_TIMER_8_BIT,
        .timer_num=FAN_TIMER,
        .freq_hz=FAN_PWM_HZ,
        .clk_cfg=LEDC_AUTO_CLK
    };
    esp_err_t err=ledc_timer_config(&timer);
    if(err!=ESP_OK) return err;
    for(unsigned i=0;i<RF_FAN_COUNT;i++) {
        ledc_channel_config_t ch={
            .gpio_num=pins[i],
            .speed_mode=LEDC_LOW_SPEED_MODE,
            .channel=channels[i],
            .intr_type=LEDC_INTR_DISABLE,
            .timer_sel=FAN_TIMER,
            .duty=255,
            .hpoint=0,
            .flags.output_invert=1
        };
        err=ledc_channel_config(&ch);
        if(err!=ESP_OK) {
            ESP_LOGE(TAG,"Fan %u PWM initialization failed: %s",i+1,esp_err_to_name(err));
            return err;
        }
    }
    ready=true;
    ESP_LOGI(TAG,"Two independent 25 kHz fan PWM outputs initialized at 100%%");
    return ESP_OK;
}
esp_err_t rf_fan_set_duty(unsigned fan_index,uint8_t percent) {
    if(!ready) return ESP_ERR_INVALID_STATE;
    if(fan_index>=RF_FAN_COUNT || percent>100) return ESP_ERR_INVALID_ARG;
    /* Minimum operational duty is 20%; 0% is deliberately forbidden until
       hardware and thermal fail-safe behaviour have been validated. */
    if(percent<20) return ESP_ERR_INVALID_ARG;
    uint32_t duty=(255U*percent+50U)/100U;
    esp_err_t err=ledc_set_duty(LEDC_LOW_SPEED_MODE,channels[fan_index],duty);
    if(err!=ESP_OK) return err;
    err=ledc_update_duty(LEDC_LOW_SPEED_MODE,channels[fan_index]);
    if(err==ESP_OK) duty_percent[fan_index]=percent;
    return err;
}
uint8_t rf_fan_get_duty(unsigned fan_index) {
    return fan_index<RF_FAN_COUNT ? duty_percent[fan_index] : 0;
}
