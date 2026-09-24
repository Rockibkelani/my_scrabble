#include "server.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int visit_count = 0;
static pthread_mutex_t visit_lock = PTHREAD_MUTEX_INITIALIZER;

void handle_api_hello(int fd) {
    const char *body = "{\"message\":\"Bonjour depuis le serveur C !\"}";
    send_response(fd, "200 OK", "application/json; charset=utf-8", body, strlen(body));
}

void handle_api_time(int fd) {
    char body[128];
    char timestr[64];
    format_now(timestr, sizeof(timestr));
    int n = snprintf(body, sizeof(body), "{\"server_time\":\"%s\"}", timestr);
    send_response(fd, "200 OK", "application/json; charset=utf-8", body, (size_t)n);
}

void handle_api_visits(int fd) {
    int current;
    pthread_mutex_lock(&visit_lock);
    visit_count++;
    current = visit_count;
    pthread_mutex_unlock(&visit_lock);

    char body[64];
    int n = snprintf(body, sizeof(body), "{\"visits\":%d}", current);
    send_response(fd, "200 OK", "application/json; charset=utf-8", body, (size_t)n);
}

void handle_api_contact(int fd, const char *req_body) {
    char decoded[4096];
    char name[256] = "", message[1024] = "";

    url_decode(decoded, sizeof(decoded), req_body);

    char *saveptr;
    char *token = strtok_r(decoded, "&", &saveptr);
    while (token) {
        char *eq = strchr(token, '=');
        if (eq) {
            *eq = '\0';
            const char *key = token;
            const char *val = eq + 1;
            if (strcmp(key, "name") == 0)
                snprintf(name, sizeof(name), "%s", val);
            else if (strcmp(key, "message") == 0)
                snprintf(message, sizeof(message), "%s", val);
        }
        token = strtok_r(NULL, "&", &saveptr);
    }

    /* Le nom et le message viennent du visiteur : on les echappe avant de les remettre dans du HTML. */
    DynBuf out;
    dynbuf_init(&out);
    dynbuf_append_str(&out,
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<title>Message envoye</title>"
        "<link rel=\"stylesheet\" href=\"/style.css\"></head><body>"
        "<main class=\"container\"><h1>Merci, ");
    html_escape_append(&out, name[0] ? name : "visiteur");
    dynbuf_append_str(&out,
        " !</h1><p>Votre message a bien ete recu par le serveur C :</p><blockquote>");
    html_escape_append(&out, message[0] ? message : "(vide)");
    dynbuf_append_str(&out,
        "</blockquote><p><a href=\"/\">Retour a l'accueil</a></p></main></body></html>");

    send_response(fd, "200 OK", "text/html; charset=utf-8", out.data, out.len);
    dynbuf_free(&out);
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
