#include "server.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void dynbuf_init(DynBuf *b) {
    b->cap = 256;
    b->len = 0;
    b->data = xmalloc(b->cap);
    b->data[0] = '\0';
}

static void dynbuf_ensure(DynBuf *b, size_t extra) {
    if (b->len + extra + 1 > b->cap) {
        while (b->cap < b->len + extra + 1) b->cap *= 2;
        b->data = xrealloc(b->data, b->cap);
    }
}

void dynbuf_append(DynBuf *b, const char *s, size_t n) {
    dynbuf_ensure(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void dynbuf_append_str(DynBuf *b, const char *s) {
    dynbuf_append(b, s, strlen(s));
}

void dynbuf_printf(DynBuf *b, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    /* Cas courant : ca tient dans la place restante (invariant : cap >= len + 1). */
    size_t avail = b->cap - b->len;
    int n = vsnprintf(b->data + b->len, avail, fmt, ap);
    va_end(ap);
    if (n >= 0 && (size_t)n >= avail) {
        dynbuf_ensure(b, (size_t)n); /* trop court : on agrandit puis on reecrit */
        vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
    }
    if (n > 0) b->len += (size_t)n;
    va_end(ap2);
}

void dynbuf_free(DynBuf *b) {
    xfree(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}
