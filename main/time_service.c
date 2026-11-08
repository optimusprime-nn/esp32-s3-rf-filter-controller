#include "time_service.h"
#include "esp_sntp.h"
#include <time.h>
#include <stdbool.h>
#include <string.h>
#define RF_NTP_SERVER_MAX 253
/* lwIP keeps these pointers rather than copying the names, so they must outlive the caller. */
static char ntp_servers[2][RF_NTP_SERVER_MAX+1];
static volatile bool time_synced;
static void time_sync_notification(struct timeval *tv) {
    time_synced=true;
}
static bool valid(const char *s) { return s && *s && strnlen(s,RF_NTP_SERVER_MAX+1)<=RF_NTP_SERVER_MAX; }
esp_err_t rf_time_service_start(const char *primary, const char *secondary) {
    if(!valid(primary) || !valid(secondary)) return ESP_ERR_INVALID_ARG;
    if(esp_sntp_enabled()) esp_sntp_stop();
    strcpy(ntp_servers[0],primary);
    strcpy(ntp_servers[1],secondary);
    time_synced=false;
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0,ntp_servers[0]);
    esp_sntp_setservername(1,ntp_servers[1]);
    sntp_set_time_sync_notification_cb(time_sync_notification);
    esp_sntp_init();
    return ESP_OK;
}
bool rf_time_service_is_synchronized(void) {
    /* esp_sntp_get_sync_status() clears the status on read, so it cannot back a repeated poll. */
    return time_synced && time(NULL)>1735689600;
}
