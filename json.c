#include "server.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void json_escape_append(DynBuf *b, const char *s) {
    char esc[8];
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
            case '"':  dynbuf_append_str(b, "\\\""); break;
            case '\\': dynbuf_append_str(b, "\\\\"); break;
            case '\n': dynbuf_append_str(b, "\\n");  break;
            case '\r': dynbuf_append_str(b, "\\r");  break;
            case '\t': dynbuf_append_str(b, "\\t");  break;
            default:
                if (c < 0x20) {
                    snprintf(esc, sizeof(esc), "\\u%04x", c);
                    dynbuf_append_str(b, esc);
                } else {
                    dynbuf_append(b, s, 1);
                }
        }
    }
}

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* Retourne un pointeur sur la valeur associee a "key", ou NULL. */
static const char *json_find_value(const char *json, const char *key) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return NULL;
    p = skip_ws(p + strlen(pattern));
    if (*p != ':') return NULL;
    return skip_ws(p + 1);
}

int json_get_int(const char *json, const char *key, long *out) {
    const char *p = json_find_value(json, key);
    if (!p) return 0;
    char *end;
    long v = strtol(p, &end, 10);
    if (end == p) return 0;
    *out = v;
    return 1;
}

int json_get_int_array(const char *json, const char *key, int *out, int max, int *n) {
    const char *p = json_find_value(json, key);
    if (!p) return 0;
    if (*p != '[') return -1;
    p++;
    int count = 0;
    for (;;) {
        p = skip_ws(p);
        if (*p == ']') break;
        if (count >= max) return -1;
        char *end;
        long v = strtol(p, &end, 10);
        if (end == p || v < INT_MIN || v > INT_MAX) return -1;
        out[count++] = (int)v;
        p = skip_ws(end);
        if (*p == ',') p++;
        else if (*p != ']') return -1;
    }
    *n = count;
    return 1;
}

const char *json_array_begin(const char *json, const char *key) {
    const char *p = json_find_value(json, key);
    if (!p || *p != '[') return NULL;
    return p + 1;
}

int json_array_next_object(const char **cursor, char *obj, size_t objsz) {
    const char *p = skip_ws(*cursor);
    if (*p == ',') p = skip_ws(p + 1);
    if (*p == ']') {
        *cursor = p + 1;
        return 0;
    }
    if (*p != '{') return -1;
    const char *q = p + 1;
    while (*q && *q != '}') {
        if (*q == '{') return -1; /* objets plats uniquement */
        q++;
    }
    if (*q != '}') return -1;
    size_t len = (size_t)(q - p) + 1;
    if (len + 1 > objsz) return -1;
    memcpy(obj, p, len);
    obj[len] = '\0';
    *cursor = q + 1;
    return 1;
}

int json_get_string(const char *json, const char *key, char *out, size_t outsize) {
    const char *p = json_find_value(json, key);
    if (!p || *p != '"' || outsize == 0) return 0;
    p++;

    size_t oi = 0;
    while (*p && *p != '"' && oi < outsize - 1) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 'n': out[oi++] = '\n'; break;
                case 't': out[oi++] = '\t'; break;
                case 'r': out[oi++] = '\r'; break;
                case '"': out[oi++] = '"';  break;
                case '\\': out[oi++] = '\\'; break;
                case '/': out[oi++] = '/';  break;
                default: out[oi++] = *p; break;
            }
            p++;
        } else {
            out[oi++] = *p++;
        }
    }
    out[oi] = '\0';
    return 1;
}

void send_json_error(int fd, const char *status, const char *message) {
    DynBuf out;
    dynbuf_init(&out);
    dynbuf_append_str(&out, "{\"error\":\"");
    json_escape_append(&out, message);
    dynbuf_append_str(&out, "\"}");
    send_response(fd, status, "application/json; charset=utf-8", out.data, out.len);
    dynbuf_free(&out);
}
