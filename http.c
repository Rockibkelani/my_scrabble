#include "server.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>

const char *content_type_for(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (strcasecmp(ext, ".html") == 0) return "text/html; charset=utf-8";
    if (strcasecmp(ext, ".css") == 0)  return "text/css; charset=utf-8";
    if (strcasecmp(ext, ".js") == 0)   return "application/javascript; charset=utf-8";
    if (strcasecmp(ext, ".json") == 0) return "application/json; charset=utf-8";
    if (strcasecmp(ext, ".png") == 0)  return "image/png";
    if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp(ext, ".svg") == 0)  return "image/svg+xml";
    if (strcasecmp(ext, ".ico") == 0)  return "image/x-icon";
    return "application/octet-stream";
}

/* Envoie tout le buffer (send peut n'en ecrire qu'une partie). MSG_NOSIGNAL : pas de SIGPIPE. */
static int send_all(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

void send_response(int fd, const char *status, const char *content_type,
                    const char *body, size_t body_len) {
    char header[512];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "Server: mini-c-server\r\n"
        "\r\n",
        status, content_type, body_len);
    if (header_len < 0 || (size_t)header_len >= sizeof(header)) return;
    if (send_all(fd, header, (size_t)header_len) != 0) return;
    if (body_len > 0) send_all(fd, body, body_len);
}

void send_404(int fd) {
    const char *body =
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<title>404</title></head><body>"
        "<h1>404 - Page non trouvee</h1>"
        "<p><a href=\"/\">Retour a l'accueil</a></p>"
        "</body></html>";
    send_response(fd, "404 Not Found", "text/html; charset=utf-8", body, strlen(body));
}

static long get_content_length(const char *headers, size_t headers_len) {
    const char *p = ci_mem_find(headers, headers_len, "\r\nContent-Length:");
    if (!p) return 0;
    p += strlen("\r\nContent-Length:");
    const char *end = headers + headers_len;
    while (p < end && *p == ' ') p++;
    return atol(p);
}

int read_http_request(int fd, char **out_buf, size_t *out_len) {
    size_t cap = 8192;
    size_t len = 0;
    char *buf = xmalloc(cap);

    const char *header_end = NULL;

    /* Phase 1 : lire jusqu'a trouver la fin des en-tetes. */
    for (;;) {
        if (len + READ_CHUNK + 1 > cap) {
            cap *= 2;
            if (cap > MAX_REQUEST_SIZE) cap = MAX_REQUEST_SIZE;
            buf = xrealloc(buf, cap);
        }
        if (len >= cap - 1) break;

        ssize_t n = recv(fd, buf + len, cap - len - 1, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            if (len == 0) { xfree(buf); return -1; }
            break;
        }
        len += (size_t)n;
        buf[len] = '\0';

        header_end = mem_find(buf, len, "\r\n\r\n");
        if (header_end || len >= MAX_REQUEST_SIZE) break;
    }

    if (header_end) {
        size_t header_total = (size_t)(header_end - buf) + 4;
        long content_length = get_content_length(buf, header_total);
        if (content_length < 0) content_length = 0;
        size_t need = (size_t)content_length;
        if (need > MAX_REQUEST_SIZE) need = MAX_REQUEST_SIZE;

        /* Phase 2 : completer le corps si necessaire. */
        while (len - header_total < need) {
            if (len + READ_CHUNK + 1 > cap) {
                size_t min_cap = header_total + need + 1;
                cap = cap * 2 > min_cap ? cap * 2 : min_cap;
                if (cap > MAX_REQUEST_SIZE + READ_CHUNK) cap = MAX_REQUEST_SIZE + READ_CHUNK;
                buf = xrealloc(buf, cap);
            }
            ssize_t n = recv(fd, buf + len, cap - len - 1, 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            len += (size_t)n;
            buf[len] = '\0';
        }
    }

    *out_buf = buf;
    *out_len = len;
    return 0;
}
