#pragma once
#include <stdbool.h>
#include <stddef.h>
typedef enum { RF_LINE_MORE, RF_LINE_DONE, RF_LINE_CANCEL, RF_LINE_OVERFLOW } rf_line_result_t;
typedef struct {
    char *buf;
    size_t cap, len;
    bool overflow, last_cr;
} rf_line_t;
void rf_line_init(rf_line_t *l, char *buf, size_t cap);
/* *erased is set when the byte removed a character, so the caller can update its echo. */
rf_line_result_t rf_line_feed(rf_line_t *l, unsigned char c, bool *erased);
void rf_secure_zero(void *p, size_t n);
