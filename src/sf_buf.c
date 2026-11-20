/*
 * sf_buf.c - Growable string buffer with JSON escaping
 *
 * Line budget constraint: < 450 lines
 */
#ifndef _CRT_SECURE_NO_WARNINGS
  #define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "sf_buf.h"

void sf_buf_add(SfBuf *b, const char *s, size_t n) {
    if (b->oom) return;
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 256;
        char *np;
        while (b->len + n + 1 > nc) nc *= 2;
        np = (char *)realloc(b->p, nc);
        if (!np) { b->oom = true; return; }
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

void sf_buf_str(SfBuf *b, const char *s) {
    sf_buf_add(b, s, strlen(s));
}

void sf_buf_printf(SfBuf *b, const char *fmt, ...) {
    char small[256];
    va_list ap, ap2;
    int n;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    n = vsnprintf(small, sizeof(small), fmt, ap);
    va_end(ap);
    if (n < 0) {
        b->oom = true;
    } else if ((size_t)n < sizeof(small)) {
        sf_buf_add(b, small, (size_t)n);
    } else {
        char *big = (char *)malloc((size_t)n + 1);
        if (!big) {
            b->oom = true;
        } else {
            vsnprintf(big, (size_t)n + 1, fmt, ap2);
            sf_buf_add(b, big, (size_t)n);
            free(big);
        }
    }
    va_end(ap2);
}

void sf_buf_json_escaped(SfBuf *b, const char *s) {
    static const char hex[] = "0123456789abcdef";
    const char *run = s;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c >= 0x20 && c != '"' && c != '\\') continue;
        if (s > run) sf_buf_add(b, run, (size_t)(s - run));
        run = s + 1;
        if (c == '"') sf_buf_str(b, "\\\"");
        else if (c == '\\') sf_buf_str(b, "\\\\");
        else if (c == '\n') sf_buf_str(b, "\\n");
        else if (c == '\r') sf_buf_str(b, "\\r");
        else if (c == '\t') sf_buf_str(b, "\\t");
        else {
            char u[7] = {'\\', 'u', '0', '0', hex[(c >> 4) & 0xF], hex[c & 0xF], '\0'};
            sf_buf_str(b, u);
        }
    }
    if (s > run) sf_buf_add(b, run, (size_t)(s - run));
}

char *sf_buf_take(SfBuf *b) {
    char *out;
    if (b->oom) { sf_buf_free(b); return NULL; }
    if (!b->p) {
        out = (char *)malloc(1);
        if (out) out[0] = '\0';
        return out;
    }
    out = b->p;
    b->p = NULL;
    b->len = b->cap = 0;
    return out;
}

void sf_buf_free(SfBuf *b) {
    if (b->p) {
        volatile unsigned char *v = (volatile unsigned char *)b->p;
        size_t n = b->cap;
        while (n--) *v++ = 0;
        free(b->p);
    }
    b->p = NULL;
    b->len = b->cap = 0;
    b->oom = false;
}
