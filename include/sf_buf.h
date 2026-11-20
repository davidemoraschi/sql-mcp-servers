/*
 * sf_buf.h - Growable string buffer with JSON escaping
 *
 * Line budget constraint: < 150 lines
 */

#ifndef SF_BUF_H
#define SF_BUF_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char *p;        /* NUL-terminated contents, NULL while empty */
    size_t len;
    size_t cap;
    bool oom;       /* sticky: set when an allocation failed     */
} SfBuf;

void sf_buf_add(SfBuf *b, const char *s, size_t n);
void sf_buf_str(SfBuf *b, const char *s);
void sf_buf_printf(SfBuf *b, const char *fmt, ...);

/* Append s JSON-escaped (no surrounding quotes). UTF-8 passes through. */
void sf_buf_json_escaped(SfBuf *b, const char *s);

/* Hand the contents to the caller (malloc'd, "" when empty) and reset.
 * Returns NULL if an allocation had failed. */
char *sf_buf_take(SfBuf *b);

/* Wipe and free the contents. */
void sf_buf_free(SfBuf *b);

#endif /* SF_BUF_H */
