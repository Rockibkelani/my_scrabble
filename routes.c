#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void handle_api_hello(int fd) {
    const char *body = "{\"message\":\"Bonjour depuis le serveur C !\"}";
    send_response(fd, "200 OK", "application/json; charset=utf-8", body, strlen(body));
}

void serve_static(int fd, const char *path) {
    if (strstr(path, "..") != NULL) {
        send_response(fd, "403 Forbidden", "text/plain", "Interdit", 8);
        return;
    }

    char filepath[1024];
    if (strcmp(path, "/") == 0) {
        snprintf(filepath, sizeof(filepath), "%s/index.html", PUBLIC_DIR);
    } else {
        snprintf(filepath, sizeof(filepath), "%s%s", PUBLIC_DIR, path);
    }

    FILE *f = fopen(filepath, "rb");
    if (!f) {
        send_404(fd);
        return;
    }

    struct stat st;
    if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode)) {
        fclose(f);
        send_404(fd);
        return;
    }
    size_t size = (size_t)st.st_size;

    char *buf = xmalloc(size + 1);
    size_t read_total = fread(buf, 1, size, f);
    fclose(f);

    send_response(fd, "200 OK", content_type_for(filepath), buf, read_total);
    xfree(buf);
}
