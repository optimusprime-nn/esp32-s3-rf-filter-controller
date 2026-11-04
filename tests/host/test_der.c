/* Host tests for the DER and PEM helpers behind the bootstrap certificate. */
#include "der_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int failures;
#define CHECK(x) do { if(!(x)) { printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); failures++; } } while(0)
static void test_wrap(void) {
    uint8_t content[300],out[310];
    size_t n=0;
    memset(content,0xAB,sizeof(content));
    CHECK(rf_der_wrap(0x04,content,0,out,sizeof(out),&n)==ESP_OK && n==2 && out[0]==0x04 && out[1]==0);
    CHECK(rf_der_wrap(0x04,content,127,out,sizeof(out),&n)==ESP_OK && n==129 && out[1]==127);
    CHECK(rf_der_wrap(0x04,content,128,out,sizeof(out),&n)==ESP_OK && n==131 && out[1]==0x81 && out[2]==128);
    CHECK(rf_der_wrap(0x04,content,255,out,sizeof(out),&n)==ESP_OK && n==258 && out[1]==0x81 && out[2]==255);
    CHECK(rf_der_wrap(0x04,content,256,out,sizeof(out),&n)==ESP_OK && n==260 && out[1]==0x82 && out[2]==1 && out[3]==0 && out[4]==0xAB);
    CHECK(rf_der_wrap(0x04,content,300,out,sizeof(out)-7,&n)==ESP_ERR_INVALID_SIZE);
    uint8_t a[]={1,2},b[]={3},c[8];
    CHECK(rf_der_concat(a,2,b,1,c,sizeof(c),&n)==ESP_OK && n==3 && c[2]==3);
    /* The certificate builder appends to its own first operand. */
    uint8_t d[8]={1,2};
    CHECK(rf_der_concat(d,2,b,1,d,sizeof(d),&n)==ESP_OK && n==3 && d[0]==1 && d[2]==3);
    CHECK(rf_der_concat(a,2,b,1,c,2,&n)==ESP_ERR_INVALID_SIZE);
}
static void test_ecdsa(void) {
    uint8_t raw[64],out[80];
    size_t n=0;
    /* r with its top bit set gains a leading zero; s with leading zero bytes loses them. */
    memset(raw,0,sizeof(raw));
    raw[0]=0x80; raw[31]=0x01;
    raw[32+30]=0x7F; raw[63]=0x02;
    CHECK(rf_ecdsa_raw_to_der(raw,out,sizeof(out),&n)==ESP_OK);
    CHECK(out[0]==0x30 && out[2]==0x02 && out[3]==33 && out[4]==0x00 && out[5]==0x80);
    size_t s_at=4+33;
    CHECK(out[s_at]==0x02 && out[s_at+1]==2 && out[s_at+2]==0x7F && out[s_at+3]==0x02);
    CHECK(n==(size_t)(2+out[1]) && out[1]==2+33+2+2);
    memset(raw,0,sizeof(raw));
    CHECK(rf_ecdsa_raw_to_der(raw,out,sizeof(out),&n)==ESP_OK && out[3]==1 && out[4]==0);
}
static void test_pem(void) {
    char *pem=NULL;
    size_t n=0;
    const uint8_t m[]={'M','a','n'};
    CHECK(rf_pem_encode("TEST",m,3,&pem,&n)==ESP_OK && !strcmp(pem,"-----BEGIN TEST-----\nTWFu\n-----END TEST-----\n") && n==strlen(pem)+1);
    free(pem);
    const uint8_t two[]={'M','a'};
    CHECK(rf_pem_encode("TEST",two,2,&pem,&n)==ESP_OK && strstr(pem,"\nTWE=\n"));
    free(pem);
    uint8_t big[100];
    for(size_t i=0;i<sizeof(big);i++) big[i]=(uint8_t)i;
    CHECK(rf_pem_encode("CERTIFICATE",big,sizeof(big),&pem,&n)==ESP_OK);
    char *body=strchr(pem,'\n')+1,*nl=strchr(body,'\n');
    CHECK(nl-body==64);
    CHECK(n==strlen(pem)+1 && strstr(pem,"-----END CERTIFICATE-----\n"));
    free(pem);
    char hex[9];
    const uint8_t h[]={0x00,0xAB,0x10,0xFF};
    rf_hex_upper(h,4,hex);
    CHECK(!strcmp(hex,"00AB10FF"));
}
int main(void) {
    test_wrap();
    test_ecdsa();
    test_pem();
    if(failures) return 1;
    puts("DER host tests PASSED");
    return 0;
}
