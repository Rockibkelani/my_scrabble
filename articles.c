#include "server.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- Modele Article + stockage en memoire (thread-safe) ---------- */

typedef struct {
    int id;
    char title[ARTICLE_TITLE_MAX];
    char content[ARTICLE_CONTENT_MAX];
    char created_at[32];
} Article;

static Article *articles = NULL;
static size_t article_count = 0;
static size_t article_capacity = 0;
static int next_article_id = 1;
static pthread_mutex_t articles_lock = PTHREAD_MUTEX_INITIALIZER;

static void articles_ensure_capacity_locked(void) {
    if (article_count >= article_capacity) {
        article_capacity = article_capacity == 0 ? 8 : article_capacity * 2;
        articles = xrealloc(articles, article_capacity * sizeof(Article));
    }
}

void articles_free(void) {
    pthread_mutex_lock(&articles_lock);
    xfree(articles);
    articles = NULL;
    article_count = 0;
    article_capacity = 0;
    pthread_mutex_unlock(&articles_lock);
}

static long article_find_index_locked(int id) {
    for (size_t i = 0; i < article_count; i++) {
        if (articles[i].id == id) return (long)i;
    }
    return -1;
}

static int article_create(const char *title, const char *content) {
    pthread_mutex_lock(&articles_lock);
    articles_ensure_capacity_locked();
    Article *a = &articles[article_count++];
    a->id = next_article_id++;
    snprintf(a->title, sizeof(a->title), "%s", title);
    snprintf(a->content, sizeof(a->content), "%s", content ? content : "");
    format_now(a->created_at, sizeof(a->created_at));
    int id = a->id;
    pthread_mutex_unlock(&articles_lock);
    return id;
}

static int article_get_copy(int id, Article *out) {
    pthread_mutex_lock(&articles_lock);
    long idx = article_find_index_locked(id);
    int found = idx >= 0;
    if (found) *out = articles[idx];
    pthread_mutex_unlock(&articles_lock);
    return found;
}

static int article_update(int id, const char *title, const char *content) {
    pthread_mutex_lock(&articles_lock);
    long idx = article_find_index_locked(id);
    int found = idx >= 0;
    if (found) {
        snprintf(articles[idx].title, sizeof(articles[idx].title), "%s", title);
        snprintf(articles[idx].content, sizeof(articles[idx].content), "%s", content);
    }
    pthread_mutex_unlock(&articles_lock);
    return found;
}

static int article_delete(int id) {
    pthread_mutex_lock(&articles_lock);
    long idx = article_find_index_locked(id);
    int found = idx >= 0;
    if (found) {
        for (size_t i = (size_t)idx; i + 1 < article_count; i++) articles[i] = articles[i + 1];
        article_count--;
    }
    pthread_mutex_unlock(&articles_lock);
    return found;
}

static void article_to_json(const Article *a, DynBuf *out) {
    char num[32];
    dynbuf_append_str(out, "{\"id\":");
    snprintf(num, sizeof(num), "%d", a->id);
    dynbuf_append_str(out, num);
    dynbuf_append_str(out, ",\"title\":\"");
    json_escape_append(out, a->title);
    dynbuf_append_str(out, "\",\"content\":\"");
    json_escape_append(out, a->content);
    dynbuf_append_str(out, "\",\"created_at\":\"");
    json_escape_append(out, a->created_at);
    dynbuf_append_str(out, "\"}");
}

static void articles_list_json(DynBuf *out) {
    pthread_mutex_lock(&articles_lock);
    dynbuf_append_str(out, "[");
    for (size_t i = 0; i < article_count; i++) {
        if (i > 0) dynbuf_append_str(out, ",");
        article_to_json(&articles[i], out);
    }
    dynbuf_append_str(out, "]");
    pthread_mutex_unlock(&articles_lock);
}

void seed_articles(void) {
    article_create("Bienvenue sur CwebSite",
                   "Ceci est le premier article, cree au demarrage du serveur.");
    article_create("Un serveur HTTP en C",
                   "Cette API REST est geree entierement par du code C, sans framework, avec un mini-parseur JSON ecrit a la main.");
}

/* ---------- Handlers /api/articles ---------- */

void handle_articles_list(int fd) {
    DynBuf out;
    dynbuf_init(&out);
    articles_list_json(&out);
    send_response(fd, "200 OK", "application/json; charset=utf-8", out.data, out.len);
    dynbuf_free(&out);
}

void handle_articles_get(int fd, int id) {
    Article a;
    if (!article_get_copy(id, &a)) {
        send_json_error(fd, "404 Not Found", "article introuvable");
        return;
    }
    DynBuf out;
    dynbuf_init(&out);
    article_to_json(&a, &out);
    send_response(fd, "200 OK", "application/json; charset=utf-8", out.data, out.len);
    dynbuf_free(&out);
}

void handle_articles_create(int fd, const char *body) {
    char title[ARTICLE_TITLE_MAX] = "";
    char content[ARTICLE_CONTENT_MAX] = "";

    if (!json_get_string(body, "title", title, sizeof(title)) || title[0] == '\0') {
        send_json_error(fd, "400 Bad Request", "le champ 'title' est requis");
        return;
    }
    json_get_string(body, "content", content, sizeof(content));

    int id = article_create(title, content);
    Article a;
    article_get_copy(id, &a);

    DynBuf out;
    dynbuf_init(&out);
    article_to_json(&a, &out);
    send_response(fd, "201 Created", "application/json; charset=utf-8", out.data, out.len);
    dynbuf_free(&out);
}

void handle_articles_update(int fd, int id, const char *body) {
    Article current;
    if (!article_get_copy(id, &current)) {
        send_json_error(fd, "404 Not Found", "article introuvable");
        return;
    }

    char title[ARTICLE_TITLE_MAX];
    char content[ARTICLE_CONTENT_MAX];
    snprintf(title, sizeof(title), "%s", current.title);
    snprintf(content, sizeof(content), "%s", current.content);

    /* Mise a jour partielle : seuls les champs presents dans le JSON sont modifies. */
    json_get_string(body, "title", title, sizeof(title));
    json_get_string(body, "content", content, sizeof(content));

    article_update(id, title, content);

    Article updated;
    article_get_copy(id, &updated);
    DynBuf out;
    dynbuf_init(&out);
    article_to_json(&updated, &out);
    send_response(fd, "200 OK", "application/json; charset=utf-8", out.data, out.len);
    dynbuf_free(&out);
}

void handle_articles_delete(int fd, int id) {
    if (!article_delete(id)) {
        send_json_error(fd, "404 Not Found", "article introuvable");
        return;
    }
    char body[64];
    int n = snprintf(body, sizeof(body), "{\"deleted\":true,\"id\":%d}", id);
    send_response(fd, "200 OK", "application/json; charset=utf-8", body, (size_t)n);
}
