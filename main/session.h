#pragma once
#include <stdbool.h>
#include <stdint.h>
#define RF_SESSION_TOKEN_LEN 64
#define RF_SESSION_IDLE_MS (15*60*1000)
/* One login at a time: a new session replaces the previous one. Times are ms since boot. */
void rf_session_create(int64_t now_ms, char out[RF_SESSION_TOKEN_LEN+1]);
bool rf_session_check(const char *token, int64_t now_ms);
void rf_session_destroy(void);
bool rf_login_allowed(int64_t now_ms);
void rf_login_result(bool success, int64_t now_ms);
