#include "server.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

int starts_with(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

const char *mem_find(const char *hay, size_t hay_len, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0 || hay_len < nlen) return NULL;
    for (size_t i = 0; i + nlen <= hay_len; i++) {
        if (memcmp(hay + i, needle, nlen) == 0) return hay + i;
    }
    return NULL;
}

const char *ci_mem_find(const char *hay, size_t hay_len, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0 || hay_len < nlen) return NULL;
    for (size_t i = 0; i + nlen <= hay_len; i++) {
        if (strncasecmp(hay + i, needle, nlen) == 0) return hay + i;
    }
    return NULL;
}

void url_decode(char *dst, size_t dstsize, const char *src) {
    if (dstsize == 0) return;
    size_t o = 0;
    char a, b;
    while (*src && o + 1 < dstsize) {
        if (*src == '%' && (a = src[1]) && (b = src[2]) &&
            isxdigit((unsigned char)a) && isxdigit((unsigned char)b)) {
            a = tolower((unsigned char)a);
            b = tolower((unsigned char)b);
            a = a >= 'a' ? a - 'a' + 10 : a - '0';
            b = b >= 'a' ? b - 'a' + 10 : b - '0';
            dst[o++] = (char)(16 * a + b);
            src += 3;
        } else if (*src == '+') {
            dst[o++] = ' ';
            src++;
        } else {
            dst[o++] = *src++;
        }
    }
    dst[o] = '\0';
}

void html_escape_append(DynBuf *b, const char *s) {
    for (; *s; s++) {
        switch (*s) {
            case '&':  dynbuf_append_str(b, "&amp;");  break;
            case '<':  dynbuf_append_str(b, "&lt;");   break;
            case '>':  dynbuf_append_str(b, "&gt;");   break;
            case '"':  dynbuf_append_str(b, "&quot;"); break;
            case '\'': dynbuf_append_str(b, "&#39;");  break;
            default:   dynbuf_append(b, s, 1);
        }
    }
}

void format_now(char *buf, size_t bufsize) {
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    strftime(buf, bufsize, "%Y-%m-%d %H:%M:%S", &tm_now);
}

int parse_id_after_prefix(const char *path, const char *prefix) {
    const char *rest = path + strlen(prefix);
    if (*rest == '\0') return -1;
    for (const char *p = rest; *p; p++) {
        if (!isdigit((unsigned char)*p)) return -1;
    }
    return atoi(rest);
}
