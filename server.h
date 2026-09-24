#ifndef SERVER_H
#define SERVER_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

/* ================= config ================= */

#define PORT_DEFAULT       8080
#define PUBLIC_DIR         "public"
#define BACKLOG            32
#define MAX_REQUEST_SIZE   (1 * 1024 * 1024) /* 1 Mo, securite anti-abus */
#define READ_CHUNK         4096

#define NUM_WORKERS        16   /* threads de traitement (pool fixe) */
#define CONN_QUEUE_SIZE    128  /* connexions en attente d'un worker */
#define IO_TIMEOUT_SECONDS 10   /* delai max de lecture/ecriture par connexion */

#define ARTICLE_TITLE_MAX   256
#define ARTICLE_CONTENT_MAX 4096

#define SCRABBLE_WORDS_FILE "data/words.txt"
#define MAX_GAMES           64    /* parties simultanees maximum */
#define GAME_TTL_SECONDS    3600  /* une partie inactive plus longtemps est liberee */

/* ================= mem (allocateur trace) =================
 * Toute la memoire du serveur passe par ces fonctions : elles comptent les
 * allocations et les liberations. mem_report() permet de verifier a l'arret
 * que rien n'a ete oublie. Une allocation impossible arrete le programme. */

void *xmalloc(size_t n);
void *xcalloc(size_t nmemb, size_t size);
void *xrealloc(void *p, size_t n);
void xfree(void *p);

/* Nombre d'allocations non liberees a cet instant. */
size_t mem_live_allocs(void);
/* Affiche le bilan ; retourne 0 si tout a ete libere, 1 sinon. */
int mem_report(FILE *out);

/* ================= dynbuf ================= */

/* Buffer de caracteres qui grandit tout seul (utilise pour construire les reponses JSON). */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} DynBuf;

void dynbuf_init(DynBuf *b);
void dynbuf_append(DynBuf *b, const char *s, size_t n);
void dynbuf_append_str(DynBuf *b, const char *s);
void dynbuf_printf(DynBuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void dynbuf_free(DynBuf *b);

/* ================= util ================= */

/* true si s commence par prefix. */
int starts_with(const char *s, const char *prefix);

/* Recherche binaire-safe (sensible a la casse) d'une sous-sequence dans un buffer. */
const char *mem_find(const char *hay, size_t hay_len, const char *needle);

/* Idem, insensible a la casse. */
const char *ci_mem_find(const char *hay, size_t hay_len, const char *needle);

/* Decode une chaine URL-encodee dans dst (taille dstsize, tronque si necessaire). */
void url_decode(char *dst, size_t dstsize, const char *src);

/* Ajoute s a b en echappant les caracteres speciaux HTML (& < > " '). */
void html_escape_append(DynBuf *b, const char *s);

/* Ecrit la date/heure courante formattee ("YYYY-MM-DD HH:MM:SS") dans buf. */
void format_now(char *buf, size_t bufsize);

/* Extrait l'entier situe apres prefix dans path ; -1 si absent ou non numerique. */
int parse_id_after_prefix(const char *path, const char *prefix);

/* ================= http ================= */

/* Devine le Content-Type a partir de l'extension du fichier. */
const char *content_type_for(const char *path);

/* Envoie une reponse HTTP complete (statut + entetes + corps) sur fd. */
void send_response(int fd, const char *status, const char *content_type,
                    const char *body, size_t body_len);

/* Envoie une page 404 HTML standard. */
void send_404(int fd);

/*
 * Lit une requete HTTP complete (entetes + corps, selon Content-Length) depuis fd.
 * Remplit out_buf (buffer alloue avec xmalloc, null-termine, a liberer avec xfree)
 * et out_len. Retourne 0 en cas de succes, -1 si la connexion n'a livre aucune donnee.
 */
int read_http_request(int fd, char **out_buf, size_t *out_len);

/* ================= json ================= */

/* Ajoute s a b en echappant les caracteres speciaux JSON ("\, \n, \r, \t, controles). */
void json_escape_append(DynBuf *b, const char *s);

/*
 * Cherche "key": "valeur" dans un objet JSON plat (valeurs chaine uniquement) et
 * copie la valeur decodee dans out. Retourne 1 si trouve, 0 sinon.
 */
int json_get_string(const char *json, const char *key, char *out, size_t outsize);

/* Lit "key": 123. Retourne 1 si trouve, 0 sinon. */
int json_get_int(const char *json, const char *key, long *out);

/*
 * Lit "key": [1, 2, 3] dans out (au plus max entrees), *n = nombre lu.
 * Retourne 1 si ok, 0 si la cle est absente, -1 si mal forme ou trop d'entrees.
 */
int json_get_int_array(const char *json, const char *key, int *out, int max, int *n);

/* Debut d'un tableau d'objets plats "key": [{...}, {...}] ; NULL si absent. */
const char *json_array_begin(const char *json, const char *key);
/*
 * Copie l'objet suivant dans obj (NUL-termine) et avance *cursor.
 * Retourne 1 (objet lu), 0 (fin du tableau) ou -1 (mal forme / trop grand).
 */
int json_array_next_object(const char **cursor, char *obj, size_t objsz);

/* Envoie une reponse d'erreur JSON {"error": "..."}. */
void send_json_error(int fd, const char *status, const char *message);

/* ================= articles ================= */

/* Cree les articles de demonstration au demarrage du serveur. */
void seed_articles(void);
/* Libere tous les articles (arret du serveur). */
void articles_free(void);

/* Handlers de la route /api/articles (et /api/articles/:id). */
void handle_articles_list(int fd);
void handle_articles_get(int fd, int id);
void handle_articles_create(int fd, const char *body);
void handle_articles_update(int fd, int id, const char *body);
void handle_articles_delete(int fd, int id);

/* ================= routes ================= */

void handle_api_hello(int fd);
void handle_api_time(int fd);
void handle_api_visits(int fd);
void handle_api_contact(int fd, const char *req_body);

/* Sert un fichier depuis PUBLIC_DIR, ou 404/403 en cas d'echec. */
void serve_static(int fd, const char *path);

/* ================= dict (dictionnaire du Scrabble) =================
 * Mots en majuscules A-Z, 2 a 15 lettres, tries. Charge une fois au demarrage,
 * ensuite en lecture seule (donc partageable entre threads sans verrou). */

#define DICT_MAX_WORD 15

typedef struct {
    char *pool;               /* tous les mots, separes par des NUL */
    const char **words;       /* words[i] pointe dans pool, tries par ordre alphabetique */
    uint8_t *lens;            /* longueur de chaque mot */
    uint32_t *masks;          /* lettres distinctes de chaque mot (bit 0 = A) */
    size_t count;
    uint32_t *pos_pool;       /* index : mots ayant la lettre L a la position P */
    uint32_t pos_off[DICT_MAX_WORD][26];
    uint32_t pos_cnt[DICT_MAX_WORD][26];
    uint32_t *len_order;      /* index : mots regroupes par longueur */
    uint32_t len_off[DICT_MAX_WORD + 2];
    uint32_t len_cnt[DICT_MAX_WORD + 2];
} Dict;

/* Retourne 0 si ok, -1 sinon (rien n'est alors a liberer). */
int dict_load(Dict *d, const char *path);
/* Libere tout ; sans danger sur un Dict deja libere ou jamais charge. */
void dict_free(Dict *d);
/* 1 si le mot (majuscules, termine par NUL) est dans le dictionnaire. */
int dict_contains(const Dict *d, const char *word);

/* ================= scrabble (logique du jeu) ================= */

#define BOARD_SIZE   15
#define RACK_SIZE    7
#define BAG_TOTAL    102   /* 100 lettres + 2 jetons blancs */
#define SUGGEST_MAX  5
#define GAME_ID_LEN  32

/* Jeton de la main / du sac. letter == 0 pour un blanc (avant d'etre pose). */
typedef struct {
    int id;
    char letter;
    unsigned char points;
    unsigned char is_blank;
} Tile;

/* Case du plateau. letter == 0 : case vide. Un blanc pose garde la lettre choisie, 0 point. */
typedef struct {
    char letter;
    unsigned char points;
    unsigned char is_blank;
} Cell;

typedef struct {
    char name[16];
    Tile rack[RACK_SIZE];
    int rack_len;
    int score;
    int is_ai;
} Player;

/* Une partie complete : un seul bloc memoire (xcalloc), libere par game_free. */
typedef struct Game {
    char id[GAME_ID_LEN + 1];
    const Dict *dict;
    Cell board[BOARD_SIZE][BOARD_SIZE];
    Tile bag[BAG_TOTAL];
    int bag_len;
    Player players[2];
    int current;
    int vs_ai;
    int first_move;
    int game_over;
    int scoreless_turns;     /* tours consecutifs sans points (passe / echange) */
    int next_tile_id;
    uint64_t rng;
    time_t last_used;
    char message[384];
} Game;

/* Lettre posee par le joueur : id du jeton, case, et lettre choisie si c'est un blanc. */
typedef struct {
    int id;
    int row;
    int col;
    char letter;
} PlayTile;

typedef struct {
    char word[DICT_MAX_WORD + 1];
    int score;
    int n;
    PlayTile tiles[RACK_SIZE];
} Suggestion;

/* seed == 0 : tirage aleatoire (getrandom). Le dictionnaire doit survivre a la partie. */
Game *game_new(const Dict *dict, int vs_ai, uint64_t seed);
void game_free(Game *g);

/* Actions : retournent 0 si ok (etat modifie, g->message mis a jour), -1 sinon
 * (message d'erreur dans err, etat de la partie strictement inchange). */
int game_play(Game *g, const PlayTile *tiles, int n, char *err, size_t errsz);
int game_exchange(Game *g, const int *ids, int n, char *err, size_t errsz);
int game_pass(Game *g, char *err, size_t errsz);
int game_end(Game *g, char *err, size_t errsz);
int game_ai_turn(Game *g, char *err, size_t errsz);

/* Meilleures suggestions pour le joueur humain courant. Retourne leur nombre,
 * ou -1 si ce n'est pas le tour d'un humain. */
int game_suggest(const Game *g, Suggestion *out, int max);

int game_ai_pending(const Game *g);           /* 1 si c'est a l'IA de jouer */
char game_premium_at(int r, int c);           /* '.', 'l' (LD), 'L' (LT), 'w' (MD), 'W' (MT) */

/* ================= scrabble_api (routes HTTP /api/scrabble) ================= */

/* Charge le dictionnaire. Retourne 0 si ok, -1 sinon (le Scrabble est alors desactive). */
int scrabble_init(const char *words_path);
/* Libere toutes les parties et le dictionnaire (arret du serveur). */
void scrabble_cleanup(void);
void scrabble_handle(int fd, const char *method, const char *path, const char *body);

#endif
