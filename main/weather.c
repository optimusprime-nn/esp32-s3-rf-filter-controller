/* CC1101 receiver for the VEVOR 7-in-1 weather station. */
#include "weather.h"
#include "cc1101_config.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include <string.h>

static const char *TAG="weather";

#ifdef CONFIG_RF_WEATHER_ENABLE

#define READ_SINGLE 0x80
#define READ_BURST 0xC0
#define WRITE_BURST 0x40
#define REG_FIFO 0x3F
#define SRES 0x30
#define SRX 0x34
#define SIDLE 0x36
#define SFRX 0x3A
#define ST_PARTNUM 0x30
#define ST_VERSION 0x31
#define ST_MARCSTATE 0x35
#define ST_RXBYTES 0x3B
#define MARCSTATE_RX 0x0D
#define RXBYTES_OVERFLOW 0x80
#ifdef CONFIG_RF_WEATHER_INVERT
#define INVERT true
#else
#define INVERT false
#endif
#define FRAME_LEN (RF_VEVOR_PACKET_LEN+2)   /* payload plus appended RSSI and LQI */
#define REPEAT_WINDOW_MS 5000
#define NVS_KEY "wx_station"

static spi_device_handle_t radio;
static SemaphoreHandle_t lock, packet_sem;
static rf_weather_status_t state={.receiver=RF_WEATHER_MISSING};
static int64_t last_ok_ms,heard_ms[RF_WEATHER_MAX_HEARD];

static int64_t now_ms(void) { return esp_timer_get_time()/1000; }

static esp_err_t xfer(const uint8_t *tx, uint8_t *rx, size_t len) {
    spi_transaction_t t={.length=len*8,.tx_buffer=tx,.rx_buffer=rx};
    return spi_device_polling_transmit(radio,&t);
}
static esp_err_t strobe(uint8_t cmd) { return xfer(&cmd,NULL,1); }
static esp_err_t write_burst(uint8_t addr, const uint8_t *v, size_t n) {
    uint8_t tx[1+RF_CC1101_CONFIG_REGS];
    if(n>RF_CC1101_CONFIG_REGS) return ESP_ERR_INVALID_SIZE;
    tx[0]=addr|WRITE_BURST;
    memcpy(tx+1,v,n);
    return xfer(tx,NULL,n+1);
}
static esp_err_t read_reg(uint8_t addr, uint8_t *v) {
    /* Status registers 0x30 to 0x3D are only reachable with the burst bit set. */
    uint8_t tx[2]={addr|(addr>=0x30?READ_BURST:READ_SINGLE),0},rx[2]={0};
    esp_err_t e=xfer(tx,rx,2);
    *v=rx[1];
    return e;
}
/* RXBYTES can be wrong if read while it changes (CC1101 errata), so read until two agree. */
static esp_err_t read_rxbytes(uint8_t *v) {
    uint8_t a,b;
    esp_err_t e=read_reg(ST_RXBYTES,&a);
    for(int i=0;e==ESP_OK && i<8;i++) {
        e=read_reg(ST_RXBYTES,&b);
        if(e==ESP_OK && a==b) { *v=a; return ESP_OK; }
        a=b;
    }
    return e==ESP_OK?ESP_ERR_INVALID_RESPONSE:e;
}
static void restart_rx(void) {
    strobe(SIDLE);
    strobe(SFRX);
    strobe(SRX);
}
static void IRAM_ATTR gdo0_isr(void *arg) {
    BaseType_t woken=pdFALSE;
    xSemaphoreGiveFromISR(packet_sem,&woken);
    if(woken) portYIELD_FROM_ISR();
}
/* Keeps the most recently heard stations, so a neighbour's station can be told apart. Lock held. */
static void note_heard(uint16_t id, int rssi, int64_t t) {
    size_t slot=0;
    for(size_t i=0;i<state.heard_count;i++) {
        if(state.heard[i].id==id) { slot=i; goto found; }
        if(heard_ms[i]<heard_ms[slot]) slot=i;
    }
    if(state.heard_count<RF_WEATHER_MAX_HEARD) slot=state.heard_count++;
found:
    state.heard[slot]=(rf_weather_heard_t){.id=id,.rssi_dbm=rssi};
    heard_ms[slot]=t;
}
static void count(uint32_t *field) {
    xSemaphoreTake(lock,portMAX_DELAY);
    (*field)++;
    xSemaphoreGive(lock);
}
static void handle_frame(uint8_t *frame) {
    if(INVERT) for(int i=0;i<RF_VEVOR_PACKET_LEN;i++) frame[i]=(uint8_t)~frame[i];
    rf_vevor_reading_t r;
    rf_vevor_result_t res=rf_vevor_decode(frame,RF_VEVOR_PACKET_LEN,&r);
    if(res!=RF_VEVOR_OK) {
        /* Noise matching the 16-bit sync word lands here too, so this is logged at debug level. */
        ESP_LOGD(TAG,"Rejected packet (%d)",(int)res);
        count(&state.packets_rejected);
        return;
    }
    int64_t t=now_ms();
    int rssi=rf_cc1101_rssi_dbm(frame[RF_VEVOR_PACKET_LEN]);
    xSemaphoreTake(lock,portMAX_DELAY);
    note_heard(r.id,rssi,t);
    if(state.sensor_id_filter>=0 && r.id!=(uint16_t)state.sensor_id_filter) {
        state.packets_ignored++;
        xSemaphoreGive(lock);
        return;
    }
    /* Each 20 s burst repeats the packet; count it once. */
    bool repeat=state.have_reading && state.last.id==r.id && state.last.counter==r.counter && t-last_ok_ms<REPEAT_WINDOW_MS;
    if(!repeat) {
        bool first=!state.have_reading;
        state.packets_ok++;
        state.have_reading=true;
        state.last=r;
        state.rssi_dbm=rssi;
        last_ok_ms=t;
        if(first) ESP_LOGI(TAG,"Weather station %04x heard, %d dBm",r.id,rssi);
    }
    xSemaphoreGive(lock);
}
static void rx_task(void *arg) {
    int64_t last_check=now_ms();
    for(;;) {
        bool signalled=xSemaphoreTake(packet_sem,pdMS_TO_TICKS(1000))==pdTRUE;
        uint8_t n=0;
        if(read_rxbytes(&n)!=ESP_OK) { restart_rx(); continue; }
        if(n&RXBYTES_OVERFLOW) { count(&state.packets_rejected); restart_rx(); continue; }
        if(n>=FRAME_LEN) {
            uint8_t tx[1+FRAME_LEN]={REG_FIFO|READ_BURST},rx[1+FRAME_LEN];
            if(xfer(tx,rx,sizeof(tx))==ESP_OK) handle_frame(rx+1);
            restart_rx();
            continue;
        }
        /* Without a packet, check every few seconds that the radio is still listening. */
        if(!signalled && now_ms()-last_check>5000) {
            last_check=now_ms();
            uint8_t marc=0;
            if(read_reg(ST_MARCSTATE,&marc)!=ESP_OK || (marc&0x1F)!=MARCSTATE_RX) {
                ESP_LOGW(TAG,"Radio left receive (state 0x%02x); restarting it",marc);
                restart_rx();
            }
        }
    }
}
esp_err_t rf_weather_start(void) {
    if(lock) return ESP_ERR_INVALID_STATE;
    lock=xSemaphoreCreateMutex();
    packet_sem=xSemaphoreCreateBinary();
    if(!lock || !packet_sem) return ESP_ERR_NO_MEM;
    state.frequency_khz=CONFIG_RF_WEATHER_FREQ_KHZ;
    state.sensor_id_filter=CONFIG_RF_WEATHER_SENSOR_ID;
    nvs_handle_t h;
    if(nvs_open("rfconfig",NVS_READONLY,&h)==ESP_OK) {
        int32_t id;
        if(nvs_get_i32(h,NVS_KEY,&id)==ESP_OK && id>=-1 && id<=0xFFFF) state.sensor_id_filter=id;
        nvs_close(h);
    }
    spi_bus_config_t bus={.mosi_io_num=CONFIG_RF_WEATHER_MOSI_GPIO,.miso_io_num=CONFIG_RF_WEATHER_MISO_GPIO,
        .sclk_io_num=CONFIG_RF_WEATHER_SCK_GPIO,.quadwp_io_num=-1,.quadhd_io_num=-1,.max_transfer_sz=64};
    esp_err_t e=spi_bus_initialize(SPI2_HOST,&bus,SPI_DMA_DISABLED);
    if(e!=ESP_OK) return e;
    spi_device_interface_config_t dev={.clock_speed_hz=4*1000*1000,.mode=0,.spics_io_num=CONFIG_RF_WEATHER_CS_GPIO,
        .queue_size=1,.cs_ena_pretrans=2};
    e=spi_bus_add_device(SPI2_HOST,&dev,&radio);
    if(e!=ESP_OK) return e;
    strobe(SRES);
    vTaskDelay(pdMS_TO_TICKS(5));
    uint8_t part=0xFF,version=0;
    read_reg(ST_PARTNUM,&part);
    read_reg(ST_VERSION,&version);
    if(part!=0x00 || version==0x00 || version==0xFF) {
        ESP_LOGW(TAG,"No CC1101 found (part 0x%02x, version 0x%02x); weather receiver off",part,version);
        return ESP_OK;
    }
    uint8_t regs[RF_CC1101_CONFIG_REGS],check[3];
    rf_cc1101_weather_config(CONFIG_RF_WEATHER_FREQ_KHZ,INVERT,regs);
    e=write_burst(0x00,regs,sizeof(regs));
    for(int i=0;e==ESP_OK && i<3;i++) e=read_reg((uint8_t)(RF_CC1101_FREQ2+i),&check[i]);
    if(e!=ESP_OK || memcmp(check,regs+RF_CC1101_FREQ2,3)!=0) {
        ESP_LOGE(TAG,"CC1101 did not accept its configuration; check the SPI wiring");
        return ESP_OK;
    }
    gpio_config_t io={.pin_bit_mask=1ULL<<CONFIG_RF_WEATHER_GDO0_GPIO,.mode=GPIO_MODE_INPUT,.intr_type=GPIO_INTR_NEGEDGE};
    e=gpio_config(&io);
    if(e==ESP_OK) { e=gpio_install_isr_service(0); if(e==ESP_ERR_INVALID_STATE) e=ESP_OK; }
    if(e==ESP_OK) e=gpio_isr_handler_add(CONFIG_RF_WEATHER_GDO0_GPIO,gdo0_isr,NULL);
    if(e!=ESP_OK) return e;
    restart_rx();
    if(xTaskCreate(rx_task,"weather",4096,NULL,4,NULL)!=pdPASS) return ESP_ERR_NO_MEM;
    xSemaphoreTake(lock,portMAX_DELAY);
    state.receiver=RF_WEATHER_RUNNING;
    state.chip_version=version;
    xSemaphoreGive(lock);
    ESP_LOGI(TAG,"CC1101 version 0x%02x listening on %lu.%03lu MHz",version,
        (unsigned long)(CONFIG_RF_WEATHER_FREQ_KHZ/1000),(unsigned long)(CONFIG_RF_WEATHER_FREQ_KHZ%1000));
    return ESP_OK;
}
void rf_weather_status(rf_weather_status_t *out) {
    if(!lock) { *out=(rf_weather_status_t){.receiver=RF_WEATHER_MISSING,.sensor_id_filter=-1,.age_ms=-1}; return; }
    int64_t t=now_ms();
    xSemaphoreTake(lock,portMAX_DELAY);
    *out=state;
    out->age_ms=state.have_reading?t-last_ok_ms:-1;
    for(size_t i=0;i<state.heard_count;i++) out->heard[i].age_ms=t-heard_ms[i];
    xSemaphoreGive(lock);
}
esp_err_t rf_weather_set_station(int32_t id) {
    if(id<-1 || id>0xFFFF) return ESP_ERR_INVALID_ARG;
    if(!lock) return ESP_ERR_INVALID_STATE;
    nvs_handle_t h;
    esp_err_t e=nvs_open("rfconfig",NVS_READWRITE,&h);
    if(e!=ESP_OK) return e;
    e=nvs_set_i32(h,NVS_KEY,id);
    if(e==ESP_OK) e=nvs_commit(h);
    nvs_close(h);
    if(e!=ESP_OK) return e;
    xSemaphoreTake(lock,portMAX_DELAY);
    state.sensor_id_filter=id;
    /* Drop a reading from a station that is now filtered out. */
    if(id>=0 && state.have_reading && state.last.id!=(uint16_t)id) state.have_reading=false;
    xSemaphoreGive(lock);
    if(id<0) ESP_LOGI(TAG,"Accepting any weather station");
    else ESP_LOGI(TAG,"Accepting only weather station %04lx",(unsigned long)id);
    return ESP_OK;
}

#else

esp_err_t rf_weather_start(void) {
    ESP_LOGI(TAG,"Weather receiver disabled in menuconfig");
    return ESP_OK;
}
void rf_weather_status(rf_weather_status_t *out) {
    *out=(rf_weather_status_t){.receiver=RF_WEATHER_DISABLED,.sensor_id_filter=-1,.age_ms=-1};
}
esp_err_t rf_weather_set_station(int32_t id) { return ESP_ERR_NOT_SUPPORTED; }

#endif

const char *rf_weather_receiver_name(rf_weather_receiver_t r) {
    switch(r) {
    case RF_WEATHER_RUNNING: return "running";
    case RF_WEATHER_MISSING: return "missing";
    default: return "disabled";
    }
}
