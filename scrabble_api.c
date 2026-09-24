/*
 * Routes HTTP du Scrabble :
 *   POST   /api/scrabble/new               {"mode":"pvp"|"ai"}      -> etat de la partie
 *   GET    /api/scrabble/:id                                        -> etat
 *   DELETE /api/scrabble/:id                                        -> libere la partie
 *   POST   /api/scrabble/:id/play          {"tiles":[{"id","row","col","letter"}]}
 *   POST   /api/scrabble/:id/suggest                                -> meilleurs mots
 *   POST   /api/scrabble/:id/exchange      {"tile_ids":[...]}
 *   POST   /api/scrabble/:id/pass | ai | end
 *
 * Toutes les parties vivent en memoire, dans un tableau de MAX_GAMES pointeurs.
 * Un verrou global protege le tableau ET les parties : une requete Scrabble
 * s'execute entierement sous ce verrou, l'envoi reseau se fait apres.
 */

#include "server.h"

#include <ctype.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>
#include <sys/random.h>

static Dict g_dict;
static int g_dict_ready = 0;
static Game *g_games[MAX_GAMES];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

int scrabble_init(const char *words_path) {
    if (dict_load(&g_dict, words_path) != 0) return -1;
    g_dict_ready = 1;
    return 0;
}

void scrabble_cleanup(void) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_GAMES; i++) {
        game_free(g_games[i]);
        g_games[i] = NULL;
    }
    pthread_mutex_unlock(&g_lock);
    if (g_dict_ready) {
        dict_free(&g_dict);
        g_dict_ready = 0;
    }
}

/* ---------- Table des parties (appelee sous verrou) ---------- */

static int new_game_id(char *out) {
    unsigned char raw[GAME_ID_LEN / 2];
    if (getrandom(raw, sizeof raw, 0) != (ssize_t)sizeof raw) return -1;
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof raw; i++) {
        out[2 * i] = hex[raw[i] >> 4];
        out[2 * i + 1] = hex[raw[i] & 15];
    }
    out[GAME_ID_LEN] = '\0';
    return 0;
}

static int valid_game_id(const char *s, size_t len) {
    if (len != GAME_ID_LEN) return 0;
    for (size_t i = 0; i < len; i++) {
        if (!isxdigit((unsigned char)s[i]) || isupper((unsigned char)s[i])) return 0;
    }
    return 1;
}

static int find_slot(const char *id) {
    for (int i = 0; i < MAX_GAMES; i++) {
        if (g_games[i] && strcmp(g_games[i]->id, id) == 0) return i;
    }
    return -1;
}

static void purge_expired(time_t now) {
    for (int i = 0; i < MAX_GAMES; i++) {
        if (g_games[i] && now - g_games[i]->last_used > GAME_TTL_SECONDS) {
            game_free(g_games[i]);
            g_games[i] = NULL;
        }
    }
}

/* ---------- JSON ---------- */

static const char *JSON_TYPE = "application/json; charset=utf-8";

static const char *json_error(DynBuf *o, const char *status, const char *msg) {
    dynbuf_append_str(o, "{\"error\":\"");
    json_escape_append(o, msg);
    dynbuf_append_str(o, "\"}");
    return status;
}

static const char *bool_str(int v) {
    return v ? "true" : "false";
}

static void append_state(DynBuf *o, const Game *g) {
    dynbuf_printf(o,
        "{\"id\":\"%s\",\"mode\":\"%s\",\"current\":%d,\"game_over\":%s,\"first_move\":%s,"
        "\"ai_pending\":%s,\"bag\":%d,\"players\":[",
        g->id, g->vs_ai ? "ai" : "pvp", g->current, bool_str(g->game_over), bool_str(g->first_move),
        bool_str(game_ai_pending(g)), g->bag_len);
    for (int p = 0; p < 2; p++) {
        dynbuf_printf(o, "%s{\"name\":\"%s\",\"score\":%d,\"ai\":%s}", p ? "," : "",
                      g->players[p].name, g->players[p].score, bool_str(g->players[p].is_ai));
    }

    /* Seule la main du joueur courant est envoyee, et jamais celle de l'IA. */
    dynbuf_append_str(o, "],\"rack\":[");
    const Player *cur = &g->players[g->current];
    if (!cur->is_ai) {
        for (int i = 0; i < cur->rack_len; i++) {
            const Tile *t = &cur->rack[i];
            char letter[2] = { t->is_blank ? '\0' : t->letter, '\0' }; /* un blanc n'a pas encore de lettre */
            dynbuf_printf(o, "%s{\"id\":%d,\"letter\":\"%s\",\"points\":%d,\"blank\":%s}", i ? "," : "",
                          t->id, letter, t->points, bool_str(t->is_blank));
        }
    }

    dynbuf_append_str(o, "],\"tiles\":[");
    int first = 1;
    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int c = 0; c < BOARD_SIZE; c++) {
            const Cell *cell = &g->board[r][c];
            if (!cell->letter) continue;
            dynbuf_printf(o, "%s{\"r\":%d,\"c\":%d,\"l\":\"%c\",\"p\":%d,\"b\":%s}", first ? "" : ",", r, c,
                          cell->letter, cell->points, bool_str(cell->is_blank));
            first = 0;
        }
    }

    dynbuf_append_str(o, "],\"premium\":[");
    for (int r = 0; r < BOARD_SIZE; r++) {
        dynbuf_append_str(o, r ? ",\"" : "\"");
        for (int c = 0; c < BOARD_SIZE; c++) {
            char ch = game_premium_at(r, c);
            dynbuf_append(o, &ch, 1);
        }
        dynbuf_append_str(o, "\"");
    }

    dynbuf_append_str(o, "],\"message\":\"");
    json_escape_append(o, g->message);
    dynbuf_append_str(o, "\"}");
}

/* Entier d'un objet JSON, borne pour que la conversion en int soit sans danger. */
static int obj_int(const char *obj, const char *key, int *out) {
    long v;
    if (!json_get_int(obj, key, &v)) return 0;
    if (v < -100000 || v > 100000) v = -1; /* hors plage : le moteur le refusera */
    *out = (int)v;
    return 1;
}

/* ---------- Handlers (appeles sous verrou ; ecrivent la reponse dans o) ---------- */

static const char *handle_new(const char *body, DynBuf *o) {
    char mode[8] = "";
    json_get_string(body, "mode", mode, sizeof mode);
    int vs_ai;
    if (strcmp(mode, "ai") == 0) vs_ai = 1;
    else if (strcmp(mode, "pvp") == 0) vs_ai = 0;
    else return json_error(o, "400 Bad Request", "Mode invalide (attendu : \"pvp\" ou \"ai\").");

    purge_expired(time(NULL));
    int slot = -1;
    for (int i = 0; i < MAX_GAMES; i++) {
        if (!g_games[i]) { slot = i; break; }
    }
    if (slot < 0) return json_error(o, "503 Service Unavailable", "Trop de parties en cours, reessayez plus tard.");

    char id[GAME_ID_LEN + 1];
    if (new_game_id(id) != 0) return json_error(o, "500 Internal Server Error", "Generation d'identifiant impossible.");

    Game *g = game_new(&g_dict, vs_ai, 0);
    memcpy(g->id, id, sizeof g->id);
    g_games[slot] = g;
    append_state(o, g);
    return "201 Created";
}

static const char *handle_play(Game *g, const char *body, DynBuf *o) {
    PlayTile tiles[RACK_SIZE];
    int n = 0;
    const char *cursor = json_array_begin(body, "tiles");
    if (!cursor) return json_error(o, "400 Bad Request", "Champ \"tiles\" requis.");

    char obj[160];
    int rc;
    while ((rc = json_array_next_object(&cursor, obj, sizeof obj)) == 1) {
        if (n >= RACK_SIZE) return json_error(o, "400 Bad Request", "Trop de lettres.");
        int id, row, col;
        if (!obj_int(obj, "id", &id) || !obj_int(obj, "row", &row) || !obj_int(obj, "col", &col)) {
            return json_error(o, "400 Bad Request", "Chaque lettre doit avoir \"id\", \"row\" et \"col\".");
        }
        char letter[4] = "";
        json_get_string(obj, "letter", letter, sizeof letter);
        tiles[n].id = id;
        tiles[n].row = row;
        tiles[n].col = col;
        tiles[n].letter = letter[0];
        n++;
    }
    if (rc < 0) return json_error(o, "400 Bad Request", "Tableau \"tiles\" mal forme.");

    char err[160];
    if (game_play(g, tiles, n, err, sizeof err) != 0) return json_error(o, "422 Unprocessable Entity", err);
    append_state(o, g);
    return "200 OK";
}

static const char *handle_exchange(Game *g, const char *body, DynBuf *o) {
    int ids[RACK_SIZE], n = 0;
    int rc = json_get_int_array(body, "tile_ids", ids, RACK_SIZE, &n);
    if (rc <= 0) return json_error(o, "400 Bad Request", "Champ \"tile_ids\" requis (tableau d'entiers).");

    char err[160];
    if (game_exchange(g, ids, n, err, sizeof err) != 0) return json_error(o, "422 Unprocessable Entity", err);
    append_state(o, g);
    return "200 OK";
}

static const char *handle_suggest(Game *g, DynBuf *o) {
    Suggestion sug[SUGGEST_MAX];
    int n = game_suggest(g, sug, SUGGEST_MAX);
    if (n < 0) return json_error(o, "422 Unprocessable Entity", "Pas de suggestion : ce n'est pas le tour d'un joueur humain.");

    dynbuf_append_str(o, "{\"suggestions\":[");
    for (int i = 0; i < n; i++) {
        dynbuf_printf(o, "%s{\"word\":\"%s\",\"score\":%d,\"tiles\":[", i ? "," : "", sug[i].word, sug[i].score);
        for (int k = 0; k < sug[i].n; k++) {
            dynbuf_printf(o, "%s{\"id\":%d,\"row\":%d,\"col\":%d,\"letter\":\"%c\"}", k ? "," : "",
                          sug[i].tiles[k].id, sug[i].tiles[k].row, sug[i].tiles[k].col, sug[i].tiles[k].letter);
        }
        dynbuf_append_str(o, "]}");
    }
    dynbuf_append_str(o, "]}");
    return "200 OK";
}

/* Actions sans corps : pass, ai, end. */
static const char *handle_simple(Game *g, const char *action, DynBuf *o) {
    char err[160];
    int rc;
    if (strcmp(action, "pass") == 0) rc = game_pass(g, err, sizeof err);
    else if (strcmp(action, "ai") == 0) rc = game_ai_turn(g, err, sizeof err);
    else rc = game_end(g, err, sizeof err);
    if (rc != 0) return json_error(o, "422 Unprocessable Entity", err);
    append_state(o, g);
    return "200 OK";
}

static const char *route(const char *method, const char *rest, const char *body, DynBuf *o) {
    if (!g_dict_ready) return json_error(o, "503 Service Unavailable", "Dictionnaire indisponible : Scrabble desactive.");

    if (strcmp(rest, "/new") == 0) {
        if (strcmp(method, "POST") != 0) return json_error(o, "405 Method Not Allowed", "Methode non autorisee.");
        return handle_new(body, o);
    }

    if (rest[0] != '/') return json_error(o, "404 Not Found", "Route inconnue.");
    const char *id = rest + 1;
    const char *slash = strchr(id, '/');
    size_t idlen = slash ? (size_t)(slash - id) : strlen(id);
    if (!valid_game_id(id, idlen)) return json_error(o, "404 Not Found", "Partie introuvable.");
    const char *action = slash ? slash + 1 : "";

    char idbuf[GAME_ID_LEN + 1];
    memcpy(idbuf, id, GAME_ID_LEN);
    idbuf[GAME_ID_LEN] = '\0';
    int slot = find_slot(idbuf);
    if (slot < 0) return json_error(o, "404 Not Found", "Partie introuvable.");
    Game *g = g_games[slot];
    g->last_used = time(NULL);

    if (action[0] == '\0') {
        if (strcmp(method, "GET") == 0) {
            append_state(o, g);
            return "200 OK";
        }
        if (strcmp(method, "DELETE") == 0) {
            game_free(g);
            g_games[slot] = NULL;
            dynbuf_append_str(o, "{\"deleted\":true}");
            return "200 OK";
        }
        return json_error(o, "405 Method Not Allowed", "Methode non autorisee.");
    }

    if (strcmp(method, "POST") != 0) return json_error(o, "405 Method Not Allowed", "Methode non autorisee.");
    if (strcmp(action, "play") == 0) return handle_play(g, body, o);
    if (strcmp(action, "exchange") == 0) return handle_exchange(g, body, o);
    if (strcmp(action, "suggest") == 0) return handle_suggest(g, o);
    if (strcmp(action, "pass") == 0 || strcmp(action, "ai") == 0 || strcmp(action, "end") == 0) {
        return handle_simple(g, action, o);
    }
    return json_error(o, "404 Not Found", "Route inconnue.");
}

void scrabble_handle(int fd, const char *method, const char *path, const char *body) {
    const char *rest = path + strlen("/api/scrabble");
    DynBuf out;
    dynbuf_init(&out);

    pthread_mutex_lock(&g_lock);
    const char *status = route(method, rest, body, &out);
    pthread_mutex_unlock(&g_lock);

    send_response(fd, status, JSON_TYPE, out.data, out.len);
    dynbuf_free(&out);
}
