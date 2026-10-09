#include "time_service.h"
#include "esp_sntp.h"
#include <time.h>
#include <stdbool.h>
#include <string.h>
esp_err_t rf_time_service_start(const char *server) {
    if(!server || !*server || strlen(server)>253) return ESP_ERR_INVALID_ARG;
    if(esp_sntp_enabled()) esp_sntp_stop();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0,(char*)server);
    esp_sntp_init();
    return ESP_OK;
}
bool rf_time_service_is_synchronized(void) {
    time_t now=time(NULL);
    /* Guard against uninitialized 1970 time; exact validity is managed by SNTP status. */
    return now>1735689600 && esp_sntp_get_sync_status()!=SNTP_SYNC_STATUS_RESET;
}
