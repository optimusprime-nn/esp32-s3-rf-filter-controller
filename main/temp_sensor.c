#include "temp_sensor.h"
#include "temp_math.h"
#include "sdkconfig.h"
#include "driver/i2c_master.h"
#include "driver/temperature_sensor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
static const char *TAG="temp";
#define SAMPLE_MS 2000
#define I2C_TIMEOUT_MS 50
#define AHT_ADDRESS 0x38
static SemaphoreHandle_t lock;
static rf_temp_reading_t readings[RF_TEMP_MAX_SENSORS];
static i2c_master_dev_handle_t devices[RF_TEMP_MAX_SENSORS];
static size_t count;
static temperature_sensor_handle_t internal;
const char *rf_temp_source_name(rf_temp_source_t s) {
    switch(s) {
    case RF_TEMP_INTERNAL: return "internal";
    case RF_TEMP_AHT: return "aht";
    default: return "lm75";
    }
}
/* AHT20 and AHT30: a measurement takes up to 80 ms, after which status bit 7 (busy) clears. */
static bool aht_read(i2c_master_dev_handle_t d, int32_t *mc, int32_t *mrh) {
    const uint8_t cmd[3]={0xAC,0x33,0x00};
    if(i2c_master_transmit(d,cmd,sizeof(cmd),I2C_TIMEOUT_MS)!=ESP_OK) return false;
    vTaskDelay(pdMS_TO_TICKS(80));
    uint8_t f[7];
    for(int tries=0;tries<5;tries++) {
        if(i2c_master_receive(d,f,sizeof(f),I2C_TIMEOUT_MS)!=ESP_OK) return false;
        if(!(f[0]&0x80)) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return rf_aht_decode(f,mc,mrh);
}
/* Aosong's register reset for parts whose status lacks 0x18 after power-on. */
static void aht_reset_register(i2c_master_dev_handle_t d, uint8_t reg) {
    uint8_t w[3]={reg,0x00,0x00},b[3];
    if(i2c_master_transmit(d,w,3,I2C_TIMEOUT_MS)!=ESP_OK) return;
    vTaskDelay(pdMS_TO_TICKS(5));
    if(i2c_master_receive(d,b,3,I2C_TIMEOUT_MS)!=ESP_OK) return;
    vTaskDelay(pdMS_TO_TICKS(10));
    w[0]=(uint8_t)(0xB0|reg); w[1]=b[1]; w[2]=b[2];
    i2c_master_transmit(d,w,3,I2C_TIMEOUT_MS);
}
static bool aht_init(i2c_master_dev_handle_t d) {
    /* Both parts need 100 ms after power-on before the first command. */
    int64_t up_ms=esp_timer_get_time()/1000;
    if(up_ms<100) vTaskDelay(pdMS_TO_TICKS(100-up_ms));
    uint8_t st;
    if(i2c_master_receive(d,&st,1,I2C_TIMEOUT_MS)!=ESP_OK) return false;
    if((st&0x18)!=0x18) {
        aht_reset_register(d,0x1B);
        aht_reset_register(d,0x1C);
        aht_reset_register(d,0x1E);
        vTaskDelay(pdMS_TO_TICKS(10));
        if(i2c_master_receive(d,&st,1,I2C_TIMEOUT_MS)!=ESP_OK) return false;
    }
    if(!(st&0x08)) {
        /* Older AHT20s: load the calibration with the initialize command. */
        const uint8_t cal[3]={0xBE,0x08,0x00};
        if(i2c_master_transmit(d,cal,sizeof(cal),I2C_TIMEOUT_MS)!=ESP_OK) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}
static void sample(void) {
    rf_temp_reading_t next[RF_TEMP_MAX_SENSORS];
    memcpy(next,readings,sizeof(next));
    for(size_t i=0;i<count;i++) {
        switch(next[i].source) {
        case RF_TEMP_INTERNAL: {
            float c;
            next[i].ok=temperature_sensor_get_celsius(internal,&c)==ESP_OK;
            if(next[i].ok) next[i].millicelsius=(int32_t)(c*1000.0f);
            break;
        }
        case RF_TEMP_AHT: {
            int32_t mc,mrh;
            next[i].ok=aht_read(devices[i],&mc,&mrh);
            if(next[i].ok) { next[i].millicelsius=mc; next[i].millipercent_rh=mrh; }
            break;
        }
        case RF_TEMP_LM75: {
            const uint8_t reg=0x00;
            uint8_t raw[2];
            next[i].ok=i2c_master_transmit_receive(devices[i],&reg,1,raw,2,I2C_TIMEOUT_MS)==ESP_OK;
            if(next[i].ok) next[i].millicelsius=rf_lm75_millicelsius(raw[0],raw[1]);
            break;
        }
        }
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
    readings[count]=(rf_temp_reading_t){.id="internal",.source=RF_TEMP_INTERNAL};
    count++;
}
static bool add_device(i2c_master_bus_handle_t bus, uint8_t bus_no, uint8_t addr, rf_temp_source_t source) {
    i2c_device_config_t dev={.dev_addr_length=I2C_ADDR_BIT_LEN_7,.device_address=addr,.scl_speed_hz=100000};
    if(i2c_master_bus_add_device(bus,&dev,&devices[count])!=ESP_OK) return false;
    rf_temp_reading_t *r=&readings[count];
    *r=(rf_temp_reading_t){.source=source,.bus=bus_no,.address=addr,.has_humidity=source==RF_TEMP_AHT};
    /* Bus 1 keeps the plain ids; bus 2 adds a 2 so both AHTs at 0x38 stay distinct. */
    snprintf(r->id,sizeof(r->id),"%s%s-0x%02x",source==RF_TEMP_AHT?"aht":"i2c",bus_no==2?"2":"",addr);
    count++;
    return true;
}
static i2c_master_bus_handle_t new_bus(int sda, int scl, uint8_t bus_no) {
    i2c_master_bus_config_t bus_cfg={.i2c_port=-1,.sda_io_num=sda,.scl_io_num=scl,
        .clk_source=I2C_CLK_SRC_DEFAULT,.glitch_ignore_cnt=7,.flags.enable_internal_pullup=true};
    i2c_master_bus_handle_t bus;
    esp_err_t e=i2c_new_master_bus(&bus_cfg,&bus);
    if(e!=ESP_OK) { ESP_LOGE(TAG,"I2C bus %u unavailable: %s",(unsigned)bus_no,esp_err_to_name(e)); return NULL; }
    return bus;
}
static bool add_aht(i2c_master_bus_handle_t bus, uint8_t bus_no) {
    if(i2c_master_probe(bus,AHT_ADDRESS,I2C_TIMEOUT_MS)!=ESP_OK || !add_device(bus,bus_no,AHT_ADDRESS,RF_TEMP_AHT)) return false;
    if(aht_init(devices[count-1])) { ESP_LOGI(TAG,"AHT20/AHT30 at 0x%02x on I2C bus %u",AHT_ADDRESS,(unsigned)bus_no); return true; }
    ESP_LOGW(TAG,"Chip at 0x%02x on I2C bus %u doesn't behave like an AHT20/AHT30",AHT_ADDRESS,(unsigned)bus_no);
    i2c_master_bus_rm_device(devices[--count]);
    return false;
}
static void add_i2c(void) {
    uint8_t addrs[RF_TEMP_MAX_LM75];
    size_t n=rf_parse_i2c_addresses(CONFIG_RF_TEMP_I2C_ADDRESSES,addrs,sizeof(addrs));
    if(!n && CONFIG_RF_TEMP_I2C_ADDRESSES[0]) ESP_LOGE(TAG,"Invalid I2C address list");
    bool aht=false;
#if CONFIG_RF_TEMP_AHT_ENABLE
    aht=true;
#endif
#if CONFIG_RF_I2C2_ENABLE
    /* The second bus holds only an AHT20/AHT30, which can't share 0x38 with another. */
    i2c_master_bus_handle_t bus2=new_bus(CONFIG_RF_I2C2_SDA_GPIO,CONFIG_RF_I2C2_SCL_GPIO,2);
    bool aht2_found=bus2 && add_aht(bus2,2);
    if(!aht2_found) ESP_LOGI(TAG,"No AHT20/AHT30 on I2C bus 2");
#endif
    if(!n && !aht) return;
    i2c_master_bus_handle_t bus=new_bus(CONFIG_RF_I2C_SDA_GPIO,CONFIG_RF_I2C_SCL_GPIO,1);
    if(!bus) return;
    bool aht_found=aht && add_aht(bus,1);
    for(size_t i=0;i<n;i++) {
        if(aht_found && addrs[i]==AHT_ADDRESS) { ESP_LOGW(TAG,"0x%02x is the AHT20/AHT30, not an LM75",addrs[i]); continue; }
        /* Only sensors present at boot are listed; one that fails later reports ok=false. */
        if(i2c_master_probe(bus,addrs[i],I2C_TIMEOUT_MS)!=ESP_OK) { ESP_LOGW(TAG,"No sensor at 0x%02x",addrs[i]); continue; }
        if(add_device(bus,1,addrs[i],RF_TEMP_LM75)) ESP_LOGI(TAG,"Temperature sensor at 0x%02x",addrs[i]);
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
