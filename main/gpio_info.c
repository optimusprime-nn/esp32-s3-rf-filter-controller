#include "gpio_info.h"
#include "fan.h"
#include "filter.h"
#include "sdr_power.h"
#include "temp_sensor.h"
#include "weather.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "soc/gpio_reg.h"
#include "soc/gpio_sig_map.h"
#include "soc/soc.h"
#include <stdio.h>
#include <string.h>
typedef struct { int gpio; const char *function, *group; } pin_use_t;
static const pin_use_t uses[]={
    {CONFIG_RF_FAN_PWM_GPIO,"Fan PWM","fan"},
    {CONFIG_RF_FAN_TACH_GPIO,"Fan tachometer","fan"},
    {CONFIG_RF_SDR_SOFT_RESET_GPIO,"SDR soft reset","sdr"},
    {CONFIG_RF_SDR_RELAY_GPIO,"SDR power relay","sdr"},
    {RF_A_GPIO,"Filter switch A","rf"},
    {RF_B_GPIO,"Filter switch B","rf"},
    {RF_MUTE_GPIO,"RF mute","rf"},
    {CONFIG_RF_I2C_SDA_GPIO,"I2C bus 1 SDA","i2c"},
    {CONFIG_RF_I2C_SCL_GPIO,"I2C bus 1 SCL","i2c"},
#if CONFIG_RF_I2C2_ENABLE
    {CONFIG_RF_I2C2_SDA_GPIO,"I2C bus 2 SDA","i2c"},
    {CONFIG_RF_I2C2_SCL_GPIO,"I2C bus 2 SCL","i2c"},
#endif
#if CONFIG_RF_WEATHER_ENABLE
    {CONFIG_RF_WEATHER_SCK_GPIO,"CC1101 SCK","weather"},
    {CONFIG_RF_WEATHER_MOSI_GPIO,"CC1101 MOSI","weather"},
    {CONFIG_RF_WEATHER_MISO_GPIO,"CC1101 MISO","weather"},
    {CONFIG_RF_WEATHER_CS_GPIO,"CC1101 CS","weather"},
    {CONFIG_RF_WEATHER_GDO0_GPIO,"CC1101 GDO0","weather"},
#endif
    {19,"USB D-","usb"},
    {20,"USB D+","usb"},
};
/* GPIO22 to 25 don't exist on the ESP32-S3. */
static bool exists(int g) { return g>=0 && g<=48 && (g<22 || g>25); }
static const char *note_for(int g) {
    if(g==0 || g==3 || g==45 || g==46) return "Strapping pin";
    if(g>=26 && g<=32) return "Flash and PSRAM";
    if(g>=33 && g<=37) return "Octal PSRAM";
    if(g==43) return "UART0 TX";
    if(g==44) return "UART0 RX";
    return NULL;
}
static int8_t level_of(int g, const gpio_io_config_t *c) {
    if(c->ie) return (int8_t)gpio_get_level((gpio_num_t)g);
    /* A plain GPIO output without input enabled: its output register is what it drives. */
    if(c->oe && c->sig_out==SIG_GPIO_OUT_IDX) {
        uint32_t r=g<32?REG_READ(GPIO_OUT_REG):REG_READ(GPIO_OUT1_REG);
        return (int8_t)((r>>(g%32))&1);
    }
    return -1;
}
static void i2c_detail(char *out, size_t len, uint8_t bus) {
    rf_temp_reading_t r[RF_TEMP_MAX_SENSORS];
    size_t n=rf_temp_snapshot(r,RF_TEMP_MAX_SENSORS),used=0,found=0;
    out[0]=0;
    for(size_t i=0;i<n;i++) {
        if(r[i].bus!=bus) continue;
        int w=snprintf(out+used,len-used,"%s%s%s",found?", ":"",r[i].id,r[i].ok?"":" (not responding)");
        if(w<0 || (size_t)w>=len-used) break;
        used+=(size_t)w; found++;
    }
    if(!found) snprintf(out,len,"No sensor found at boot");
}
static void detail_for(const pin_use_t *u, int8_t level, char *out, size_t len) {
    out[0]=0;
    if(u->gpio==CONFIG_RF_FAN_PWM_GPIO) snprintf(out,len,"%u%% duty",(unsigned)rf_fan_percent());
    else if(u->gpio==CONFIG_RF_FAN_TACH_GPIO) {
        int32_t rpm=rf_fan_rpm_now();
        if(rpm<0) snprintf(out,len,"No reading"); else snprintf(out,len,"%ld RPM",(long)rpm);
    }
    else if(u->gpio==CONFIG_RF_SDR_SOFT_RESET_GPIO) {
        rf_sdr_status_t s; rf_sdr_status(&s);
        snprintf(out,len,"%s",s.state==RF_SDR_SOFT_RESET?"Pulling low: Radxa rebooting":"Released");
    }
    else if(u->gpio==CONFIG_RF_SDR_RELAY_GPIO) {
        rf_sdr_status_t s; rf_sdr_status(&s);
        snprintf(out,len,"%s",s.state==RF_SDR_POWER_OFF?"Energised: Radxa power off":"Idle: Radxa powered");
    }
    else if(u->gpio==RF_A_GPIO || u->gpio==RF_B_GPIO) snprintf(out,len,"Filter %u selected",(unsigned)rf_filter_current());
    else if(u->gpio==RF_MUTE_GPIO) snprintf(out,len,"%s",level==1?"Muted (held on)":level==0?"Not muted":"");
    else if(u->gpio==CONFIG_RF_I2C_SDA_GPIO || u->gpio==CONFIG_RF_I2C_SCL_GPIO) i2c_detail(out,len,1);
#if CONFIG_RF_I2C2_ENABLE
    else if(u->gpio==CONFIG_RF_I2C2_SDA_GPIO || u->gpio==CONFIG_RF_I2C2_SCL_GPIO) i2c_detail(out,len,2);
#endif
#if CONFIG_RF_WEATHER_ENABLE
    else if(!strcmp(u->group,"weather")) {
        rf_weather_status_t w; rf_weather_status(&w);
        snprintf(out,len,"Receiver %s",rf_weather_receiver_name(w.receiver));
    }
#endif
    else if(!strcmp(u->group,"usb")) snprintf(out,len,"Console and management (COM port)");
}
size_t rf_gpio_info(rf_gpio_info_t *out, size_t max) {
    size_t n=0;
    for(int g=0;g<=48 && n<max;g++) {
        if(!exists(g)) continue;
        rf_gpio_info_t *p=&out[n++];
        memset(p,0,sizeof(*p));
        p->gpio=(uint8_t)g;
        p->note=note_for(g);
        p->level=-1;
        const pin_use_t *u=NULL;
        for(size_t i=0;i<sizeof(uses)/sizeof(uses[0]);i++) if(uses[i].gpio==g) { u=&uses[i]; break; }
        if(u) { p->function=u->function; p->group=u->group; }
        else if(g>=26 && g<=37) { p->group="flash"; continue; }
        gpio_io_config_t c;
        if(gpio_get_io_config((gpio_num_t)g,&c)!=ESP_OK) continue;
        p->config_read=true;
        p->input=c.ie; p->output=c.oe; p->open_drain=c.od; p->pull_up=c.pu; p->pull_down=c.pd;
        p->drive=(uint8_t)c.drv;
        p->peripheral=c.oe && c.sig_out!=SIG_GPIO_OUT_IDX;
        /* The USB pins belong to the USB PHY rather than the GPIO matrix. */
        if(g==19 || g==20) p->peripheral=true;
        p->level=level_of(g,&c);
        if(u) detail_for(u,p->level,p->detail,sizeof(p->detail));
    }
    return n;
}
