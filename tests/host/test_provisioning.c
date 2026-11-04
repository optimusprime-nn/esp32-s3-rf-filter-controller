/* Host tests for Wi-Fi and TLS provisioning and the console line editor. */
#include "provisioning.h"
#include "console_line.h"
#include "nvs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define SLOTS 8
#define NS_WIFI 0
#define NS_TLS 1
static struct { char name[32]; unsigned char *data; size_t len; } slots[SLOTS];
static const char *namespaces[]={"wifi","tls"};
static bool ns_exists[2];
static int slot(nvs_handle_t h, const char *key, bool create) {
    char name[32];
    snprintf(name,sizeof(name),"%u/%s",(unsigned)h,key);
    for(int i=0;i<SLOTS;i++) if(!strcmp(slots[i].name,name)) return i;
    if(create) for(int i=0;i<SLOTS;i++) if(!slots[i].name[0]) { strcpy(slots[i].name,name); return i; }
    return -1;
}
esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *h) {
    for(unsigned i=0;i<2;i++) if(!strcmp(ns,namespaces[i])) {
        if(mode==NVS_READWRITE) ns_exists[i]=true;
        if(!ns_exists[i]) return ESP_ERR_NVS_NOT_FOUND;
        *h=i; return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len) {
    int i=slot(h,key,false);
    if(i<0) return ESP_ERR_NVS_NOT_FOUND;
    if(out) memcpy(out,slots[i].data,slots[i].len<*len?slots[i].len:*len);
    *len=slots[i].len;
    return ESP_OK;
}
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len) { return nvs_get_blob(h,key,out,len); }
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *in, size_t len) {
    int i=slot(h,key,true);
    free(slots[i].data);
    slots[i].data=malloc(len);
    memcpy(slots[i].data,in,len);
    slots[i].len=len;
    return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *in) { return nvs_set_blob(h,key,in,strlen(in)+1); }
esp_err_t nvs_erase_all(nvs_handle_t h) {
    char prefix[8];
    snprintf(prefix,sizeof(prefix),"%u/",(unsigned)h);
    for(int i=0;i<SLOTS;i++) if(!strncmp(slots[i].name,prefix,strlen(prefix))) { free(slots[i].data); slots[i].data=NULL; slots[i].len=0; slots[i].name[0]='\0'; }
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { return ESP_OK; }
void nvs_close(nvs_handle_t h) {}
static int failures;
#define CHECK(x) do { if(!(x)) { printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); failures++; } } while(0)
static const char *stored(unsigned ns, const char *key) {
    int i=slot(ns,key,false);
    return i<0?NULL:(const char*)slots[i].data;
}
static void test_wifi(void) {
    char longer[70];
    CHECK(rf_provision_set_wifi("","password1")==ESP_ERR_INVALID_ARG);
    CHECK(rf_provision_set_wifi("net","short")==ESP_ERR_INVALID_ARG);
    CHECK(rf_provision_set_wifi("123456789012345678901234567890123","password1")==ESP_ERR_INVALID_ARG);
    CHECK(rf_provision_set_wifi("net","pass\tword1")==ESP_ERR_INVALID_ARG);
    memset(longer,'z',64); longer[64]='\0';
    CHECK(rf_provision_set_wifi("net",longer)==ESP_ERR_INVALID_ARG);
    memset(longer,'a',64);
    CHECK(rf_provision_set_wifi("net",longer)==ESP_OK);
    longer[64]='a'; longer[65]='\0';
    CHECK(rf_provision_set_wifi("net",longer)==ESP_ERR_INVALID_ARG);
    CHECK(rf_provision_set_wifi("my network","pass word1")==ESP_OK);
    CHECK(!strcmp(stored(NS_WIFI,"ssid"),"my network") && !strcmp(stored(NS_WIFI,"password"),"pass word1"));
}
static void test_tls(void) {
    const char *cert="-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE-----\n";
    const char *key="-----BEGIN PRIVATE KEY-----\nBBBB\n-----END PRIVATE KEY-----\n";
    rf_provision_status_t s;
    CHECK(rf_provision_set_tls("not pem at all, not even close to it",key)==ESP_ERR_INVALID_ARG);
    CHECK(rf_provision_set_tls(cert,"-----BEGIN PRIVATE KEY-----\ntruncated, no end marker")==ESP_ERR_INVALID_ARG);
    CHECK(stored(NS_TLS,"cert")==NULL);
    CHECK(rf_provision_set_tls(cert,key)==ESP_OK);
    size_t n=0;
    CHECK(nvs_get_blob(NS_TLS,"cert",NULL,&n)==ESP_OK && n==strlen(cert)+1 && stored(NS_TLS,"cert")[n-1]=='\0');
    rf_provision_get_status(&s);
    CHECK(s.tls);
    CHECK(rf_provision_clear_tls()==ESP_OK);
    rf_provision_get_status(&s);
    CHECK(!s.tls && stored(NS_TLS,"key")==NULL);
    CHECK(rf_provision_set_tls(cert,key)==ESP_OK);
}
static rf_line_result_t feed(rf_line_t *l, const char *s) {
    rf_line_result_t r=RF_LINE_MORE;
    bool erased;
    for(;*s && r==RF_LINE_MORE;s++) r=rf_line_feed(l,(unsigned char)*s,&erased);
    return r;
}
static void test_line(void) {
    char buf[8];
    rf_line_t l;
    bool erased;
    rf_line_init(&l,buf,sizeof(buf));
    CHECK(feed(&l,"ab\bc\r")==RF_LINE_DONE && !strcmp(buf,"ac"));
    rf_line_init(&l,buf,sizeof(buf));
    l.last_cr=true;
    CHECK(rf_line_feed(&l,'\n',&erased)==RF_LINE_MORE && l.len==0);
    CHECK(feed(&l,"x\n")==RF_LINE_DONE && !strcmp(buf,"x"));
    rf_line_init(&l,buf,sizeof(buf));
    CHECK(feed(&l,"\b\x1b y\x7f\r")==RF_LINE_DONE && !strcmp(buf," "));
    rf_line_init(&l,buf,sizeof(buf));
    CHECK(feed(&l,"abcdefghij\r")==RF_LINE_OVERFLOW && strlen(buf)==7);
    rf_line_init(&l,buf,sizeof(buf));
    CHECK(feed(&l,"secret\x03")==RF_LINE_CANCEL);
}
int main(void) {
    rf_provision_status_t s;
    rf_provision_get_status(&s);
    CHECK(!s.wifi && !s.tls);
    test_wifi();
    test_tls();
    test_line();
    rf_provision_get_status(&s);
    CHECK(s.wifi && s.tls);
    for(int i=0;i<SLOTS;i++) free(slots[i].data);
    if(failures) return 1;
    puts("provisioning host tests PASSED");
    return 0;
}
