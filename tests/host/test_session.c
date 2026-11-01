/* Host tests for the HTTPS session token and login backoff. */
#include "session.h"
#include "esp_random.h"
#include <stdio.h>
#include <string.h>
static unsigned char next_byte;
void esp_fill_random(void *buf, size_t len) {
    unsigned char *p=buf;
    while(len--) *p++=next_byte++;
}
static int failures;
#define CHECK(x) do { if(!(x)) { printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); failures++; } } while(0)
static void test_session(void) {
    char a[RF_SESSION_TOKEN_LEN+1],b[RF_SESSION_TOKEN_LEN+1];
    CHECK(!rf_session_check("",0));
    rf_session_create(1000,a);
    CHECK(strlen(a)==RF_SESSION_TOKEN_LEN);
    CHECK(rf_session_check(a,1000));
    CHECK(!rf_session_check(NULL,1000));
    CHECK(!rf_session_check("short",1000));
    memcpy(b,a,sizeof(a));
    b[RF_SESSION_TOKEN_LEN-1]^=1;
    CHECK(!rf_session_check(b,1000));
    CHECK(rf_session_check(a,1000+RF_SESSION_IDLE_MS));
    CHECK(rf_session_check(a,1000+RF_SESSION_IDLE_MS*2));
    CHECK(!rf_session_check(a,1001+RF_SESSION_IDLE_MS*3));
    CHECK(!rf_session_check(a,1000+RF_SESSION_IDLE_MS*3));
    rf_session_create(5000,a);
    rf_session_create(5000,b);
    CHECK(strcmp(a,b)!=0 && !rf_session_check(a,5000) && rf_session_check(b,5000));
    rf_session_destroy();
    CHECK(!rf_session_check(b,5000));
}
static void test_backoff(void) {
    int64_t now=1000000;
    for(int i=0;i<3;i++) { CHECK(rf_login_allowed(now)); rf_login_result(false,now); }
    CHECK(rf_login_allowed(now));
    rf_login_result(false,now);
    CHECK(!rf_login_allowed(now+999) && rf_login_allowed(now+1000));
    rf_login_result(false,now);
    CHECK(!rf_login_allowed(now+1999) && rf_login_allowed(now+2000));
    for(int i=0;i<100;i++) rf_login_result(false,now);
    CHECK(!rf_login_allowed(now+299999) && rf_login_allowed(now+300000));
    rf_login_result(true,now);
    CHECK(rf_login_allowed(now));
}
int main(void) {
    test_session();
    test_backoff();
    if(failures) return 1;
    puts("session host tests PASSED");
    return 0;
}
