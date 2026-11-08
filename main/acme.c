#include "acme.h"
#include "acme_core.h"
#include "cloudflare_client.h"
#include "management_settings.h"
#include "provisioning.h"
#include "time_service.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/x509_crt.h"
#include "nvs.h"
#include "psa/crypto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static const char *TAG="acme";
extern esp_err_t web_restart(void);
#define NS "acme"
#define TOKEN_MAX 256
#define TASK_STACK 20480
#define RENEW_DAYS 30
#define RENEW_CHECK_MS (6ULL*3600*1000)
/* Cloudflare's authoritative servers usually answer within seconds; Let's Encrypt asks them directly. */
#define PROPAGATION_MS 15000

static SemaphoreHandle_t lock;
static rf_acme_state_t state=RF_ACME_IDLE;
static bool run_production;
static char stage[24]="",last_error[120]="";
static int64_t started,finished;

const char *rf_acme_state_name(rf_acme_state_t s) {
    switch(s) {
    case RF_ACME_RUNNING: return "running";
    case RF_ACME_DONE: return "done";
    case RF_ACME_FAILED: return "failed";
    default: return "idle";
    }
}
static void set_stage(const char *s) {
    xSemaphoreTake(lock,portMAX_DELAY);
    snprintf(stage,sizeof(stage),"%s",s);
    xSemaphoreGive(lock);
    ESP_LOGI(TAG,"Certificate request: %s",s);
}
static void finish(esp_err_t e, const char *what, int http_status) {
    xSemaphoreTake(lock,portMAX_DELAY);
    finished=time(NULL);
    if(e==ESP_OK) { state=RF_ACME_DONE; last_error[0]=0; snprintf(stage,sizeof(stage),"done"); }
    else {
        state=RF_ACME_FAILED;
        if(http_status) snprintf(last_error,sizeof(last_error),"%s failed: %s (HTTP %d)",what,esp_err_to_name(e),http_status);
        else snprintf(last_error,sizeof(last_error),"%s failed: %s",what,esp_err_to_name(e));
    }
    xSemaphoreGive(lock);
    if(e==ESP_OK) ESP_LOGI(TAG,"Certificate issued and in use");
    else ESP_LOGE(TAG,"Certificate request stopped: %s",last_error);
}

/* ---- NVS ---- */
static esp_err_t nvs_str(const char *key, char *out, size_t cap) {
    nvs_handle_t h;
    esp_err_t e=nvs_open(NS,NVS_READONLY,&h);
    if(e!=ESP_OK) return e;
    size_t n=cap;
    e=nvs_get_str(h,key,out,&n);
    nvs_close(h);
    return e;
}
static esp_err_t nvs_u8(const char *key, uint8_t *out) {
    nvs_handle_t h;
    esp_err_t e=nvs_open(NS,NVS_READONLY,&h);
    if(e!=ESP_OK) return e;
    e=nvs_get_u8(h,key,out);
    nvs_close(h);
    return e;
}
esp_err_t acme_core_store_certificate(bool production, const char *hostname, const char *chain_pem, const char *key_pem) {
    esp_err_t e=rf_provision_set_tls(chain_pem,key_pem);
    if(e!=ESP_OK) return e;
    nvs_handle_t h;
    e=nvs_open(NS,NVS_READWRITE,&h);
    if(e!=ESP_OK) return e;
    e=nvs_set_str(h,"cert_host",hostname);
    if(e==ESP_OK) e=nvs_set_u8(h,"cert_prod",production?1:0);
    if(e==ESP_OK) e=nvs_commit(h);
    nvs_close(h);
    return e;
}
esp_err_t rf_acme_set_cloudflare(const char *token, const char *zone_name, int *http_status) {
    if(!token || !zone_name || !token[0] || strlen(token)>TOKEN_MAX || !acme_core_dns_name_valid(zone_name)) return ESP_ERR_INVALID_ARG;
    char zone_id[33];
    int status=0;
    esp_err_t e=cloudflare_client_resolve_zone(token,zone_name,zone_id,sizeof(zone_id),&status);
    if(http_status) *http_status=status;
    if(e!=ESP_OK) return e;
    nvs_handle_t h;
    e=nvs_open(NS,NVS_READWRITE,&h);
    if(e!=ESP_OK) return e;
    e=nvs_set_str(h,"cf_token",token);
    if(e==ESP_OK) e=nvs_set_str(h,"cf_zone_id",zone_id);
    if(e==ESP_OK) e=nvs_commit(h);
    nvs_close(h);
    if(e!=ESP_OK) return e;
    rf_management_settings_t s;
    e=rf_settings_load(&s);
    if(e==ESP_OK) { snprintf(s.cloudflare_zone,sizeof(s.cloudflare_zone),"%s",zone_name); e=rf_settings_save(&s); }
    if(e==ESP_OK) ESP_LOGI(TAG,"Cloudflare token stored for zone %s",zone_name);
    return e;
}
esp_err_t rf_acme_clear_cloudflare(void) {
    nvs_handle_t h;
    esp_err_t e=nvs_open(NS,NVS_READWRITE,&h);
    if(e!=ESP_OK) return e;
    nvs_erase_key(h,"cf_token");
    nvs_erase_key(h,"cf_zone_id");
    e=nvs_commit(h);
    nvs_close(h);
    rf_management_settings_t s;
    if(e==ESP_OK && rf_settings_load(&s)==ESP_OK && s.acme_enabled) { s.acme_enabled=false; e=rf_settings_save(&s); }
    return e;
}

/* ---- The stored certificate ---- */
static int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y-=m<=2;
    const int64_t era=(y>=0?y:y-399)/400;
    const unsigned yoe=(unsigned)(y-era*400);
    const unsigned doy=(153*(m+(m>2?-3:9))+2)/5+d-1;
    const unsigned doe=yoe*365+yoe/4-yoe/100+doy;
    return era*146097+(int64_t)doe-719468;
}
static int64_t x509_unix(const mbedtls_x509_time *t) {
    return days_from_civil(t->year,(unsigned)t->mon,(unsigned)t->day)*86400+t->hour*3600+t->min*60+t->sec;
}
static void cert_info(rf_acme_status_t *o) {
    nvs_handle_t h;
    if(nvs_open("tls",NVS_READONLY,&h)!=ESP_OK) return;
    size_t n=0;
    char *pem=NULL;
    if(nvs_get_blob(h,"cert",NULL,&n)==ESP_OK && n>1 && (pem=malloc(n)) && nvs_get_blob(h,"cert",pem,&n)!=ESP_OK) { free(pem); pem=NULL; }
    nvs_close(h);
    if(!pem) return;
    pem[n-1]=0;
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    if(!mbedtls_x509_crt_parse(&crt,(const unsigned char*)pem,n)) {
        o->cert_present=true;
        mbedtls_x509_dn_gets(o->cert_issuer,sizeof(o->cert_issuer),&crt.issuer);
        snprintf(o->cert_valid_from,sizeof(o->cert_valid_from),"%04d-%02d-%02dT%02d:%02d:%02dZ",crt.valid_from.year,
            crt.valid_from.mon,crt.valid_from.day,crt.valid_from.hour,crt.valid_from.min,crt.valid_from.sec);
        snprintf(o->cert_valid_to,sizeof(o->cert_valid_to),"%04d-%02d-%02dT%02d:%02d:%02dZ",crt.valid_to.year,
            crt.valid_to.mon,crt.valid_to.day,crt.valid_to.hour,crt.valid_to.min,crt.valid_to.sec);
        o->cert_expires=x509_unix(&crt.valid_to);
        time_t now=time(NULL);
        o->cert_days_left=rf_time_service_is_synchronized()?(int)((o->cert_expires-(int64_t)now)/86400):-1;
        uint8_t d[32];
        size_t dn=0;
        if(psa_hash_compute(PSA_ALG_SHA_256,crt.raw.p,crt.raw.len,d,sizeof(d),&dn)==PSA_SUCCESS && dn==32)
            for(int i=0;i<32;i++) snprintf(o->cert_sha256+i*2,3,"%02X",d[i]);
        char host[254];
        uint8_t prod=0;
        if(nvs_str("cert_host",host,sizeof(host))==ESP_OK) {
            o->cert_from_acme=true;
            snprintf(o->cert_hostname,sizeof(o->cert_hostname),"%s",host);
            o->cert_production=nvs_u8("cert_prod",&prod)==ESP_OK && prod;
        } else mbedtls_x509_dn_gets(o->cert_hostname,sizeof(o->cert_hostname),&crt.subject);
    }
    mbedtls_x509_crt_free(&crt);
    free(pem);
}
void rf_acme_status(rf_acme_status_t *o) {
    memset(o,0,sizeof(*o));
    xSemaphoreTake(lock,portMAX_DELAY);
    o->state=state; o->production=run_production; o->started=started; o->finished=finished;
    snprintf(o->stage,sizeof(o->stage),"%s",stage);
    snprintf(o->error,sizeof(o->error),"%s",last_error);
    xSemaphoreGive(lock);
    char token[TOKEN_MAX+1];
    o->token_present=nvs_str("cf_token",token,sizeof(token))==ESP_OK && token[0];
    memset(token,0,sizeof(token));
    if(nvs_str("cf_zone_id",o->zone_id,sizeof(o->zone_id))!=ESP_OK) o->zone_id[0]=0;
    cert_info(o);
}
/* A certificate set or removed by hand replaces an issued one. */
void rf_acme_forget_certificate(void) {
    nvs_handle_t h;
    if(nvs_open(NS,NVS_READWRITE,&h)!=ESP_OK) return;
    nvs_erase_key(h,"cert_host");
    nvs_erase_key(h,"cert_prod");
    nvs_commit(h);
    nvs_close(h);
}

/* ---- Issuance ---- */
typedef struct {
    bool production;
    char fqdn[RF_FQDN_MAX+1];
    char token[TOKEN_MAX+1];
    char zone_id[33];
} job_t;
static void acme_task(void *arg) {
    job_t *j=arg;
    int http=0;
    esp_err_t e;
    acme_account_status_t *acct=calloc(1,sizeof(*acct));
    acme_order_discovery_t *order=calloc(1,sizeof(*order));
    acme_challenge_validation_t *val=calloc(1,sizeof(*val));
    acme_certificate_issue_status_t *cert=calloc(1,sizeof(*cert));
    char record_id[CLOUDFLARE_DNS_RECORD_ID_LENGTH+1]="";
    if(!acct || !order || !val || !cert) { finish(ESP_ERR_NO_MEM,"Starting",0); goto out; }

    set_stage("account");
    e=acme_core_provision_account(j->production,acct);
    if(e!=ESP_OK) { finish(e,"Registering the ACME account",acct->http_status); goto out; }

    set_stage("order");
    e=acme_core_discover_order(j->production,j->fqdn,order);
    if(e!=ESP_OK) { finish(e,"Creating the order",order->authorization_http_status?order->authorization_http_status:order->new_order_http_status); goto out; }

    /* An authorization Let's Encrypt still holds as valid needs no new challenge. */
    if(strcmp(order->challenge_status,"valid")) {
        set_stage("dns");
        e=cloudflare_client_create_dns01_txt(j->token,j->zone_id,order->dns01_record_name,order->dns01_value,record_id,sizeof(record_id),&http);
        if(e!=ESP_OK) { finish(e,"Creating the DNS TXT record",http); goto out; }
        e=cloudflare_client_verify_dns01_txt_content(j->token,j->zone_id,record_id,order->dns01_record_name,order->dns01_value,&http);
        if(e!=ESP_OK) { finish(e,"Checking the DNS TXT record",http); goto cleanup; }
        vTaskDelay(pdMS_TO_TICKS(PROPAGATION_MS));
        set_stage("challenge");
        e=acme_core_validate_dns01(j->production,order,val);
        if(e!=ESP_OK) { finish(e,"Validating the DNS challenge",val->poll_http_status?val->poll_http_status:val->trigger_http_status); goto cleanup; }
    }

    set_stage("finalize");
    e=acme_core_finalize_order(j->production,order,cert);
    if(e!=ESP_OK) {
        int s=cert->certificate_http_status?cert->certificate_http_status:cert->order_poll_http_status?cert->order_poll_http_status:cert->finalize_http_status;
        finish(e,cert->certificate_retrieved?"Storing the certificate":"Finalizing the order",s);
        goto cleanup;
    }
    if(j->production) {
        rf_management_settings_t s;
        if(rf_settings_load(&s)==ESP_OK && !s.acme_enabled) { s.acme_enabled=true; rf_settings_save(&s); }
    }
    e=web_restart();
    if(e!=ESP_OK) ESP_LOGE(TAG,"HTTPS restart failed (%s); the certificate is used from the next boot",esp_err_to_name(e));
    finish(ESP_OK,NULL,0);

cleanup:
    if(record_id[0]) {
        set_stage("cleanup");
        esp_err_t c=cloudflare_client_delete_dns01_txt_content(j->token,j->zone_id,record_id,order->dns01_record_name,order->dns01_value,&http);
        if(c!=ESP_OK) ESP_LOGW(TAG,"Could not delete the TXT record %s (%s); remove it in Cloudflare",order->dns01_record_name,esp_err_to_name(c));
        xSemaphoreTake(lock,portMAX_DELAY);
        snprintf(stage,sizeof(stage),"%s",state==RF_ACME_DONE?"done":"failed");
        xSemaphoreGive(lock);
    }
out:
    memset(j->token,0,sizeof(j->token));
    free(j); free(acct); free(order); free(val); free(cert);
    vTaskDelete(NULL);
}
esp_err_t rf_acme_issue(bool production) {
    rf_management_settings_t s;
    if(rf_settings_load(&s)!=ESP_OK || !strchr(s.fqdn,'.') || !acme_core_dns_name_valid(s.fqdn) || !s.cloudflare_zone[0]) return ESP_ERR_INVALID_STATE;
    size_t fl=strlen(s.fqdn),zl=strlen(s.cloudflare_zone);
    if(!(fl==zl && !strcmp(s.fqdn,s.cloudflare_zone)) && !(fl>zl && s.fqdn[fl-zl-1]=='.' && !strcmp(s.fqdn+fl-zl,s.cloudflare_zone)))
        return ESP_ERR_INVALID_ARG;
    if(!rf_time_service_is_synchronized()) return ESP_ERR_TIMEOUT;
    job_t *j=calloc(1,sizeof(*j));
    if(!j) return ESP_ERR_NO_MEM;
    j->production=production;
    snprintf(j->fqdn,sizeof(j->fqdn),"%s",s.fqdn);
    if(nvs_str("cf_token",j->token,sizeof(j->token))!=ESP_OK || nvs_str("cf_zone_id",j->zone_id,sizeof(j->zone_id))!=ESP_OK) {
        free(j); return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(lock,portMAX_DELAY);
    if(state==RF_ACME_RUNNING) { xSemaphoreGive(lock); memset(j,0,sizeof(*j)); free(j); return ESP_ERR_INVALID_STATE; }
    state=RF_ACME_RUNNING; run_production=production; started=time(NULL); finished=0;
    snprintf(stage,sizeof(stage),"starting"); last_error[0]=0;
    xSemaphoreGive(lock);
    ESP_LOGI(TAG,"Requesting a %s certificate for %s",production?"Let's Encrypt":"Let's Encrypt staging",j->fqdn);
    if(xTaskCreate(acme_task,"acme",TASK_STACK,j,4,NULL)!=pdPASS) {
        memset(j,0,sizeof(*j)); free(j);
        finish(ESP_ERR_NO_MEM,"Starting",0);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* Renews an issued production certificate 30 days before it expires. */
static void renew_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(10*60*1000));
    for(;;) {
        rf_management_settings_t s;
        rf_acme_status_t *st=calloc(1,sizeof(*st));
        if(st && rf_settings_load(&s)==ESP_OK && s.acme_enabled && rf_time_service_is_synchronized()) {
            rf_acme_status(st);
            bool due=!st->cert_present || (st->cert_from_acme && st->cert_production && st->cert_days_left>=0 && st->cert_days_left<RENEW_DAYS)
                || (st->cert_from_acme && strcmp(st->cert_hostname,s.fqdn));
            if(due && st->token_present && st->state!=RF_ACME_RUNNING) {
                ESP_LOGI(TAG,"Automatic renewal: %s",st->cert_present?"certificate due":"no certificate");
                esp_err_t e=rf_acme_issue(true);
                if(e!=ESP_OK) ESP_LOGW(TAG,"Automatic renewal not started: %s",esp_err_to_name(e));
            }
        }
        free(st);
        vTaskDelay(pdMS_TO_TICKS(RENEW_CHECK_MS));
    }
}
esp_err_t rf_acme_start(void) {
    lock=xSemaphoreCreateMutex();
    if(!lock) return ESP_ERR_NO_MEM;
    return xTaskCreate(renew_task,"acme_renew",6144,NULL,2,NULL)==pdPASS?ESP_OK:ESP_ERR_NO_MEM;
}
