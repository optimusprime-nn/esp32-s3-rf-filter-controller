#include "console_line.h"
#define CTRL_C 0x03
void rf_line_init(rf_line_t *l, char *buf, size_t cap) {
    *l=(rf_line_t){.buf=buf,.cap=cap};
    buf[0]='\0';
}
rf_line_result_t rf_line_feed(rf_line_t *l, unsigned char c, bool *erased) {
    bool after_cr=l->last_cr;
    l->last_cr=c=='\r';
    *erased=false;
    if(c==CTRL_C) return RF_LINE_CANCEL;
    if(c=='\n' && after_cr) return RF_LINE_MORE;
    if(c=='\r' || c=='\n') return l->overflow?RF_LINE_OVERFLOW:RF_LINE_DONE;
    if(c=='\b' || c==0x7f) {
        if(l->len && !l->overflow) { l->buf[--l->len]='\0'; *erased=true; }
        return RF_LINE_MORE;
    }
    if(c<0x20) return RF_LINE_MORE;
    if(l->len+1>=l->cap) { l->overflow=true; return RF_LINE_MORE; }
    l->buf[l->len++]=(char)c;
    l->buf[l->len]='\0';
    return RF_LINE_MORE;
}
void rf_secure_zero(void *p, size_t n) {
    volatile unsigned char *v=p;
    while(n--) *v++=0;
}
