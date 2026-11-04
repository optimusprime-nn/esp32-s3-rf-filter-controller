#include "temp_sensor.h"
#include "temp_math.h"
#include "sdkconfig.h"
#include "driver/i2c_master.h"
#include "driver/temperature_sensor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
static const char *TAG="temp";
#define SAMPLE_MS 2000
#define I2C_TIMEOUT_MS 50
static SemaphoreHandle_t lock;
static rf_temp_reading_t readings[RF_TEMP_MAX_SENSORS];
static i2c_master_dev_handle_t devices[RF_TEMP_MAX_SENSORS];
static size_t count;
static temperature_sensor_handle_t internal;
static void sample(void) {
    rf_temp_reading_t next[RF_TEMP_MAX_SENSORS];
    memcpy(next,readings,sizeof(next));
    for(size_t i=0;i<count;i++) {
        if(next[i].internal) {
            float c;
            next[i].ok=temperature_sensor_get_celsius(internal,&c)==ESP_OK;
            if(next[i].ok) next[i].millicelsius=(int32_t)(c*1000.0f);
            continue;
        }
        const uint8_t reg=0x00;
        uint8_t raw[2];
        next[i].ok=i2c_master_transmit_receive(devices[i],&reg,1,raw,2,I2C_TIMEOUT_MS)==ESP_OK;
        if(next[i].ok) next[i].millicelsius=rf_lm75_millicelsius(raw[0],raw[1]);
    }
    xSemaphoreTake(lock,portMAX_DELAY);
    memcpy(readings,next,sizeof(readings));
    xSemaphoreGive(lock);
}
static void sample_task(void *arg) {
    for(;;) { sample(); vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS)); }
}
static void add_internal(void) {
    temperature_sensor_config_t cfg=TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10,80);
    esp_err_t e=temperature_sensor_install(&cfg,&internal);
    if(e==ESP_OK) e=temperature_sensor_enable(internal);
    if(e!=ESP_OK) { ESP_LOGW(TAG,"Internal sensor unavailable: %s",esp_err_to_name(e)); return; }
    readings[count]=(rf_temp_reading_t){.id="internal",.internal=true};
    count++;
}
static void add_i2c(void) {
    uint8_t addrs[RF_TEMP_MAX_SENSORS-1];
    size_t n=rf_parse_i2c_addresses(CONFIG_RF_TEMP_I2C_ADDRESSES,addrs,sizeof(addrs));
    if(!n) { if(CONFIG_RF_TEMP_I2C_ADDRESSES[0]) ESP_LOGE(TAG,"Invalid I2C address list"); return; }
    i2c_master_bus_config_t bus_cfg={.i2c_port=-1,.sda_io_num=CONFIG_RF_I2C_SDA_GPIO,.scl_io_num=CONFIG_RF_I2C_SCL_GPIO,
        .clk_source=I2C_CLK_SRC_DEFAULT,.glitch_ignore_cnt=7,.flags.enable_internal_pullup=true};
    i2c_master_bus_handle_t bus;
    esp_err_t e=i2c_new_master_bus(&bus_cfg,&bus);
    if(e!=ESP_OK) { ESP_LOGE(TAG,"I2C bus unavailable: %s",esp_err_to_name(e)); return; }
    for(size_t i=0;i<n;i++) {
        /* Only sensors present at boot are listed; one that fails later reports ok=false. */
        if(i2c_master_probe(bus,addrs[i],I2C_TIMEOUT_MS)!=ESP_OK) { ESP_LOGW(TAG,"No sensor at 0x%02x",addrs[i]); continue; }
        i2c_device_config_t dev={.dev_addr_length=I2C_ADDR_BIT_LEN_7,.device_address=addrs[i],.scl_speed_hz=100000};
        if(i2c_master_bus_add_device(bus,&dev,&devices[count])!=ESP_OK) continue;
        rf_temp_reading_t *r=&readings[count];
        *r=(rf_temp_reading_t){.address=addrs[i]};
        snprintf(r->id,sizeof(r->id),"i2c-0x%02x",addrs[i]);
        count++;
        ESP_LOGI(TAG,"Temperature sensor at 0x%02x",addrs[i]);
    }
}
esp_err_t rf_temp_start(void) {
    if(lock) return ESP_ERR_INVALID_STATE;
    lock=xSemaphoreCreateMutex();
    if(!lock) return ESP_ERR_NO_MEM;
    add_internal();
    add_i2c();
    sample();
    if(xTaskCreate(sample_task,"temp",4096,NULL,3,NULL)!=pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}
size_t rf_temp_snapshot(rf_temp_reading_t *out, size_t max) {
    if(!lock) return 0;
    xSemaphoreTake(lock,portMAX_DELAY);
    size_t n=count<max?count:max;
    memcpy(out,readings,n*sizeof(*out));
    xSemaphoreGive(lock);
    return n;
}
