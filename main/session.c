#include "session.h"
#include "esp_random.h"
#include <stdio.h>
#include <string.h>
#define FREE_ATTEMPTS 3
#define MAX_LOCKOUT_MS 300000
static char session_token[RF_SESSION_TOKEN_LEN+1];
static int64_t last_used_ms;
static volatile bool session_active;
static unsigned failures;
static int64_t locked_until_ms;
void rf_session_create(int64_t now_ms, char out[RF_SESSION_TOKEN_LEN+1]) {
    uint8_t raw[RF_SESSION_TOKEN_LEN/2];
    session_active=false;
    esp_fill_random(raw,sizeof(raw));
    for(size_t i=0;i<sizeof(raw);i++) snprintf(session_token+i*2,3,"%02x",raw[i]);
    last_used_ms=now_ms;
    session_active=true;
    memcpy(out,session_token,sizeof(session_token));
}
bool rf_session_check(const char *token, int64_t now_ms) {
    if(!session_active || !token || strlen(token)!=RF_SESSION_TOKEN_LEN) return false;
    if(now_ms-last_used_ms>RF_SESSION_IDLE_MS) { session_active=false; return false; }
    unsigned char diff=0;
    for(size_t i=0;i<RF_SESSION_TOKEN_LEN;i++) diff|=(unsigned char)(token[i]^session_token[i]);
    if(diff) return false;
    last_used_ms=now_ms;
    return true;
}
void rf_session_destroy(void) {
    session_active=false;
}
bool rf_login_allowed(int64_t now_ms) {
    return now_ms>=locked_until_ms;
}
void rf_login_result(bool success, int64_t now_ms) {
    if(success) { failures=0; locked_until_ms=0; return; }
    failures++;
    if(failures<=FREE_ATTEMPTS) return;
    unsigned shift=failures-FREE_ATTEMPTS-1;
    int64_t lockout=shift>=9?MAX_LOCKOUT_MS:1000LL<<shift;
    locked_until_ms=now_ms+(lockout>MAX_LOCKOUT_MS?MAX_LOCKOUT_MS:lockout);
}
