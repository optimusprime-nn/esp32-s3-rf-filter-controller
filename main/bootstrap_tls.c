/* Ported from esp32p4_nts_time_server (acme_client.c, "Per-board bootstrap TLS identity"). */
#include "bootstrap_tls.h"
#include "console_line.h"
#include "der_util.h"
#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/x509_crt.h"
#include "nvs.h"
#include "psa/crypto.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
static const char *TAG="bootstrap_tls";
#define NVS_NS "boot_tls"
#define CERT_MAX 768
static uint8_t cert_der[CERT_MAX];
static size_t cert_der_len;
static uint8_t private_key[32];
static bool ready;
static esp_err_t sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
    size_t n=0;
    return psa_hash_compute(PSA_ALG_SHA_256,data,len,out,32,&n)==PSA_SUCCESS && n==32?ESP_OK:ESP_FAIL;
}
static esp_err_t generate_key(uint8_t priv[32], uint8_t pub[65]) {
    psa_key_attributes_t attr=PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr,PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr,256);
    psa_set_key_usage_flags(&attr,PSA_KEY_USAGE_EXPORT|PSA_KEY_USAGE_SIGN_HASH);
    psa_set_key_algorithm(&attr,PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    psa_key_id_t id=0;
    psa_status_t s=psa_generate_key(&attr,&id);
    psa_reset_key_attributes(&attr);
    if(s!=PSA_SUCCESS) return ESP_FAIL;
    size_t pl=0,ql=0;
    s=psa_export_key(id,priv,32,&pl);
    if(s==PSA_SUCCESS) s=psa_export_public_key(id,pub,65,&ql);
    psa_destroy_key(id);
    if(s!=PSA_SUCCESS || pl!=32 || ql!=65 || pub[0]!=0x04) { rf_secure_zero(priv,32); return ESP_FAIL; }
    return ESP_OK;
}
static esp_err_t sign(const uint8_t priv[32], const uint8_t digest[32], uint8_t sig[64]) {
    psa_key_attributes_t attr=PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr,PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr,256);
    psa_set_key_usage_flags(&attr,PSA_KEY_USAGE_SIGN_HASH);
    psa_set_key_algorithm(&attr,PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    psa_key_id_t id=0;
    psa_status_t s=psa_import_key(&attr,priv,32,&id);
    psa_reset_key_attributes(&attr);
    if(s!=PSA_SUCCESS) return ESP_FAIL;
    size_t n=0;
    s=psa_sign_hash(id,PSA_ALG_ECDSA(PSA_ALG_SHA_256),digest,32,sig,64,&n);
    psa_destroy_key(id);
    return s==PSA_SUCCESS && n==64?ESP_OK:ESP_FAIL;
}
static esp_err_t build_cert(const uint8_t priv[32], const uint8_t pub[65], uint8_t *out, size_t out_size, size_t *out_len) {
    static const char cn[]="esp32s3-rf-bootstrap";
    static const uint8_t oid_cn[]={0x06,0x03,0x55,0x04,0x03};
    static const uint8_t oid_ec_pub[]={0x06,0x07,0x2A,0x86,0x48,0xCE,0x3D,0x02,0x01};
    static const uint8_t oid_p256[]={0x06,0x08,0x2A,0x86,0x48,0xCE,0x3D,0x03,0x01,0x07};
    static const uint8_t oid_ecdsa_sha256[]={0x06,0x08,0x2A,0x86,0x48,0xCE,0x3D,0x04,0x03,0x02};
    static const uint8_t oid_san[]={0x06,0x03,0x55,0x1D,0x11};
    static const uint8_t version3[]={0xA0,0x03,0x02,0x01,0x02};
    /* 2024-01-01 to 2049-12-31. Clients trust this certificate by fingerprint, so the clock doesn't matter. */
    static const uint8_t validity[]={0x30,0x1E,
        0x17,0x0D,'2','4','0','1','0','1','0','0','0','0','0','0','Z',
        0x17,0x0D,'4','9','1','2','3','1','2','3','5','9','5','9','Z'};
    uint8_t a[CERT_MAX],b[CERT_MAX],c[CERT_MAX];
    size_t al,bl,cl;
    uint8_t name[64],spki[128],exts[80],serial[20],sig_alg[16];
    size_t name_len,spki_len,exts_len,serial_len,sig_alg_len;
    /* Name: CN=cn, used for both issuer and subject. */
    esp_err_t e=rf_der_wrap(0x0C,(const uint8_t*)cn,strlen(cn),a,sizeof(a),&al);
    if(e==ESP_OK) e=rf_der_concat(oid_cn,sizeof(oid_cn),a,al,b,sizeof(b),&bl);
    if(e==ESP_OK) e=rf_der_wrap(0x30,b,bl,a,sizeof(a),&al);
    if(e==ESP_OK) e=rf_der_wrap(0x31,a,al,b,sizeof(b),&bl);
    if(e==ESP_OK) e=rf_der_wrap(0x30,b,bl,name,sizeof(name),&name_len);
    /* SubjectPublicKeyInfo. */
    uint8_t bitpub[66]={0};
    memcpy(bitpub+1,pub,65);
    if(e==ESP_OK) e=rf_der_concat(oid_ec_pub,sizeof(oid_ec_pub),oid_p256,sizeof(oid_p256),a,sizeof(a),&al);
    if(e==ESP_OK) e=rf_der_wrap(0x30,a,al,b,sizeof(b),&bl);
    if(e==ESP_OK) e=rf_der_wrap(0x03,bitpub,sizeof(bitpub),c,sizeof(c),&cl);
    if(e==ESP_OK) e=rf_der_concat(b,bl,c,cl,a,sizeof(a),&al);
    if(e==ESP_OK) e=rf_der_wrap(0x30,a,al,spki,sizeof(spki),&spki_len);
    /* Extensions [3]: subjectAltName dNSName=cn. */
    if(e==ESP_OK) e=rf_der_wrap(0x82,(const uint8_t*)cn,strlen(cn),a,sizeof(a),&al);
    if(e==ESP_OK) e=rf_der_wrap(0x30,a,al,b,sizeof(b),&bl);
    if(e==ESP_OK) e=rf_der_wrap(0x04,b,bl,c,sizeof(c),&cl);
    if(e==ESP_OK) e=rf_der_concat(oid_san,sizeof(oid_san),c,cl,a,sizeof(a),&al);
    if(e==ESP_OK) e=rf_der_wrap(0x30,a,al,b,sizeof(b),&bl);
    if(e==ESP_OK) e=rf_der_wrap(0x30,b,bl,c,sizeof(c),&cl);
    if(e==ESP_OK) e=rf_der_wrap(0xA3,c,cl,exts,sizeof(exts),&exts_len);
    /* Positive 16-byte random serial. */
    uint8_t serial_value[16];
    esp_fill_random(serial_value,sizeof(serial_value));
    serial_value[0]=(serial_value[0]&0x7F)|0x01;
    if(e==ESP_OK) e=rf_der_wrap(0x02,serial_value,sizeof(serial_value),serial,sizeof(serial),&serial_len);
    if(e==ESP_OK) e=rf_der_wrap(0x30,oid_ecdsa_sha256,sizeof(oid_ecdsa_sha256),sig_alg,sizeof(sig_alg),&sig_alg_len);
    if(e!=ESP_OK) return e;
    const struct { const uint8_t *p; size_t n; } parts[]={
        {version3,sizeof(version3)},{serial,serial_len},{sig_alg,sig_alg_len},{name,name_len},
        {validity,sizeof(validity)},{name,name_len},{spki,spki_len},{exts,exts_len}};
    size_t used=0;
    for(size_t i=0;i<sizeof(parts)/sizeof(parts[0]);i++) {
        if(used+parts[i].n>sizeof(a)) return ESP_ERR_INVALID_SIZE;
        memcpy(a+used,parts[i].p,parts[i].n);
        used+=parts[i].n;
    }
    e=rf_der_wrap(0x30,a,used,b,sizeof(b),&bl);
    uint8_t digest[32],raw_sig[64],der_sig[80],sigbits[81];
    size_t der_sig_len=0;
    if(e==ESP_OK) e=sha256(b,bl,digest);
    if(e==ESP_OK) e=sign(priv,digest,raw_sig);
    if(e==ESP_OK) e=rf_ecdsa_raw_to_der(raw_sig,der_sig,sizeof(der_sig),&der_sig_len);
    rf_secure_zero(raw_sig,sizeof(raw_sig));
    if(e!=ESP_OK) return e;
    sigbits[0]=0;
    memcpy(sigbits+1,der_sig,der_sig_len);
    e=rf_der_wrap(0x03,sigbits,der_sig_len+1,c,sizeof(c),&cl);
    if(e==ESP_OK) e=rf_der_concat(b,bl,sig_alg,sig_alg_len,a,sizeof(a),&al);
    if(e==ESP_OK) e=rf_der_concat(a,al,c,cl,a,sizeof(a),&al);
    if(e==ESP_OK) e=rf_der_wrap(0x30,a,al,out,out_size,out_len);
    return e;
}
static esp_err_t ensure(void) {
    if(ready) return ESP_OK;
    if(psa_crypto_init()!=PSA_SUCCESS) return ESP_FAIL;
    nvs_handle_t h;
    esp_err_t e=nvs_open(NVS_NS,NVS_READWRITE,&h);
    if(e!=ESP_OK) return e;
    size_t dl=sizeof(cert_der),kl=sizeof(private_key);
    if(nvs_get_blob(h,"crt",cert_der,&dl)==ESP_OK && nvs_get_blob(h,"key",private_key,&kl)==ESP_OK && kl==32 && dl>0) {
        cert_der_len=dl;
    } else {
        uint8_t pub[65];
        e=generate_key(private_key,pub);
        if(e==ESP_OK) e=build_cert(private_key,pub,cert_der,sizeof(cert_der),&cert_der_len);
        if(e==ESP_OK) {
            /* Never store or serve a certificate mbedTLS cannot parse. */
            mbedtls_x509_crt check;
            mbedtls_x509_crt_init(&check);
            if(mbedtls_x509_crt_parse_der(&check,cert_der,cert_der_len)) e=ESP_ERR_INVALID_RESPONSE;
            mbedtls_x509_crt_free(&check);
        }
        if(e==ESP_OK) e=nvs_set_blob(h,"crt",cert_der,cert_der_len);
        if(e==ESP_OK) e=nvs_set_blob(h,"key",private_key,sizeof(private_key));
        if(e==ESP_OK) e=nvs_commit(h);
        if(e!=ESP_OK) { rf_secure_zero(private_key,sizeof(private_key)); nvs_close(h); ESP_LOGE(TAG,"Bootstrap certificate generation failed: %s",esp_err_to_name(e)); return e; }
        ESP_LOGI(TAG,"Generated this board's bootstrap TLS certificate");
    }
    nvs_close(h);
    ready=true;
    return ESP_OK;
}
esp_err_t rf_bootstrap_tls_load(char **cert_pem, size_t *cert_len, char **key_pem, size_t *key_len) {
    *cert_pem=*key_pem=NULL;
    esp_err_t e=ensure();
    if(e!=ESP_OK) return e;
    /* SEC1 ECPrivateKey: version 1, the 32-byte scalar, and the P-256 curve OID. */
    uint8_t key_der[51]={0x30,0x31,0x02,0x01,0x01,0x04,0x20};
    static const uint8_t curve[]={0xA0,0x0A,0x06,0x08,0x2A,0x86,0x48,0xCE,0x3D,0x03,0x01,0x07};
    memcpy(key_der+7,private_key,32);
    memcpy(key_der+39,curve,sizeof(curve));
    e=rf_pem_encode("CERTIFICATE",cert_der,cert_der_len,cert_pem,cert_len);
    if(e==ESP_OK) e=rf_pem_encode("EC PRIVATE KEY",key_der,sizeof(key_der),key_pem,key_len);
    rf_secure_zero(key_der,sizeof(key_der));
    if(e!=ESP_OK) { free(*cert_pem); *cert_pem=NULL; }
    return e;
}
esp_err_t rf_bootstrap_tls_sha256(char out_hex[65]) {
    uint8_t d[32];
    esp_err_t e=ensure();
    if(e==ESP_OK) e=sha256(cert_der,cert_der_len,d);
    if(e==ESP_OK) rf_hex_upper(d,sizeof(d),out_hex);
    return e;
}
