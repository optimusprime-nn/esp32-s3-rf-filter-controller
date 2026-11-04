#include "der_util.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char b64[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static esp_err_t put_length(uint8_t *out, size_t out_size, size_t length, size_t *written) {
    if(length<128) { if(out_size<1) return ESP_ERR_INVALID_SIZE; out[0]=(uint8_t)length; *written=1; return ESP_OK; }
    if(length<=255) { if(out_size<2) return ESP_ERR_INVALID_SIZE; out[0]=0x81; out[1]=(uint8_t)length; *written=2; return ESP_OK; }
    if(length<=65535) {
        if(out_size<3) return ESP_ERR_INVALID_SIZE;
        out[0]=0x82; out[1]=(uint8_t)(length>>8); out[2]=(uint8_t)length; *written=3; return ESP_OK;
    }
    return ESP_ERR_INVALID_SIZE;
}
esp_err_t rf_der_wrap(uint8_t tag, const uint8_t *content, size_t content_len, uint8_t *out, size_t out_size, size_t *out_len) {
    if(!out || !out_len || (content_len && !content)) return ESP_ERR_INVALID_ARG;
    if(out_size<2) return ESP_ERR_INVALID_SIZE;
    size_t ll=0;
    out[0]=tag;
    if(put_length(out+1,out_size-1,content_len,&ll)!=ESP_OK || 1+ll+content_len>out_size) return ESP_ERR_INVALID_SIZE;
    if(content_len) memmove(out+1+ll,content,content_len);
    *out_len=1+ll+content_len;
    return ESP_OK;
}
esp_err_t rf_der_concat(const uint8_t *a, size_t al, const uint8_t *b, size_t bl, uint8_t *out, size_t out_size, size_t *out_len) {
    if(al+bl>out_size) return ESP_ERR_INVALID_SIZE;
    memmove(out,a,al);
    memmove(out+al,b,bl);
    *out_len=al+bl;
    return ESP_OK;
}
esp_err_t rf_ecdsa_raw_to_der(const uint8_t raw[64], uint8_t *out, size_t out_size, size_t *out_len) {
    uint8_t ints[72];
    size_t used=0;
    for(unsigned part=0;part<2;part++) {
        const uint8_t *v=raw+part*32;
        size_t off=0;
        while(off<31 && !v[off]) off++;
        size_t n=32-off;
        bool lead_zero=v[off]&0x80;
        ints[used++]=0x02;
        ints[used++]=(uint8_t)(n+(lead_zero?1:0));
        if(lead_zero) ints[used++]=0;
        memcpy(ints+used,v+off,n);
        used+=n;
    }
    return rf_der_wrap(0x30,ints,used,out,out_size,out_len);
}
esp_err_t rf_pem_encode(const char *label, const uint8_t *der, size_t der_len, char **out_pem, size_t *out_len) {
    size_t b64_len=(der_len+2)/3*4;
    size_t total=2*strlen(label)+32+b64_len+b64_len/64+2;
    char *pem=calloc(1,total);
    if(!pem) return ESP_ERR_NO_MEM;
    size_t o=(size_t)snprintf(pem,total,"-----BEGIN %s-----\n",label),line=0;
    for(size_t i=0;i<der_len;i+=3) {
        uint32_t v=(uint32_t)der[i]<<16|(uint32_t)(i+1<der_len?der[i+1]:0)<<8|(i+2<der_len?der[i+2]:0);
        pem[o++]=b64[v>>18&63];
        pem[o++]=b64[v>>12&63];
        pem[o++]=i+1<der_len?b64[v>>6&63]:'=';
        pem[o++]=i+2<der_len?b64[v&63]:'=';
        if((line+=4)==64) { pem[o++]='\n'; line=0; }
    }
    if(line) pem[o++]='\n';
    o+=(size_t)snprintf(pem+o,total-o,"-----END %s-----\n",label);
    *out_pem=pem;
    *out_len=o+1;
    return ESP_OK;
}
void rf_hex_upper(const uint8_t *in, size_t n, char *out) {
    static const char hex[]="0123456789ABCDEF";
    for(size_t i=0;i<n;i++) { out[i*2]=hex[in[i]>>4]; out[i*2+1]=hex[in[i]&15]; }
    out[n*2]='\0';
}
