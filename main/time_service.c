#include "time_service.h"
#include "esp_sntp.h"
#include <time.h>
#include <stdbool.h>
#include <string.h>
#define RF_NTP_SERVER_MAX 253
/* lwIP keeps this pointer rather than copying the name, so it must outlive the caller. */
static char ntp_server[RF_NTP_SERVER_MAX+1];
static volatile bool time_synced;
static void time_sync_notification(struct timeval *tv) {
    time_synced=true;
}
esp_err_t rf_time_service_start(const char *server) {
    if(!server || !*server || strnlen(server,RF_NTP_SERVER_MAX+1)>RF_NTP_SERVER_MAX) return ESP_ERR_INVALID_ARG;
    if(esp_sntp_enabled()) esp_sntp_stop();
    strcpy(ntp_server,server);
    time_synced=false;
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0,ntp_server);
    sntp_set_time_sync_notification_cb(time_sync_notification);
    esp_sntp_init();
    return ESP_OK;
}
bool rf_time_service_is_synchronized(void) {
    /* esp_sntp_get_sync_status() clears the status on read, so it cannot back a repeated poll. */
    return time_synced && time(NULL)>1735689600;
}
