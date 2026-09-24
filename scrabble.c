/*
 * Logique du Scrabble (plateau 15x15, sac de 102 jetons, regles francaises).
 * Aucune dependance reseau : ce module ne connait ni HTTP ni JSON.
 * Seule allocation dynamique : la structure Game elle-meme (game_new / game_free).
 * Tout le reste vit sur la pile (plateaux de travail, recherche de l'IA...).
 */

#include "server.h"

#include <ctype.h>
#include <string.h>
#include <sys/random.h>

#define MAX_NEW        RACK_SIZE
#define BINGO_BONUS    50
#define MAX_SCORELESS_TURNS 6 /* regle officielle : 6 tours de suite sans points terminent la partie */

/* Placements essayes au maximum par recherche : borne le temps de reponse (~0,5 s au pire
   en build optimise). Mesure : sans plafond, le pire cas observe reste sous cette limite. */
#ifndef AI_SEARCH_BUDGET
#define AI_SEARCH_BUDGET 40000000L
#endif

static const int LETTER_VALUES[26] = {
    1, 3, 3, 2, 1, 4, 2, 4, 1, 8, 10, 1, 2, 1, 1, 3, 8, 1, 1, 1, 1, 4, 10, 10, 10, 10,
};

static const int LETTER_COUNTS[26] = {
    9, 2, 2, 3, 15, 2, 2, 2, 8, 1, 1, 5, 3, 6, 6, 2, 1, 6, 6, 6, 6, 2, 1, 1, 1, 1,
};

/* '.' rien, 'l' lettre double, 'L' lettre triple, 'w' mot double, 'W' mot triple. */
static const char PREMIUM_MAP[BOARD_SIZE][BOARD_SIZE + 1] = {
    "W..l...W...l..W",
    ".w...L...L...w.",
    "..w...l.l...w..",
    "l..w...l...w..l",
    "....w.....w....",
    ".L...L...L...L.",
    "..l...l.l...l..",
    "W..l...w...l..W",
    "..l...l.l...l..",
    ".L...L...L...L.",
    "....w.....w....",
    "l..w...l...w..l",
    "..w...l.l...w..",
    ".w...L...L...w.",
    "W..l...W...l..W",
};

char game_premium_at(int r, int c) {
    return PREMIUM_MAP[r][c];
}

/* ---------- Generateur aleatoire (xorshift64*) ---------- */

static uint64_t random_seed(void) {
    uint64_t s = 0;
    if (getrandom(&s, sizeof s, 0) != (ssize_t)sizeof s) s = (uint64_t)time(NULL) * 0x9E3779B97F4A7C15ULL;
    if (s == 0) s = 0x9E3779B97F4A7C15ULL;
    return s;
}

static uint64_t rng_next(uint64_t *s) {
    uint64_t x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 0x2545F4914F6CDD1DULL;
}

static void shuffle_bag(Game *g) {
    for (int i = g->bag_len - 1; i > 0; i--) {
        int j = (int)(rng_next(&g->rng) % (uint64_t)(i + 1));
        Tile t = g->bag[i];
        g->bag[i] = g->bag[j];
        g->bag[j] = t;
    }
}

/* ---------- Sac et mains ---------- */

static int draw_tile(Game *g, Tile *out) {
    if (g->bag_len == 0) return 0;
    Tile t = g->bag[--g->bag_len];
    t.id = g->next_tile_id++;
    *out = t;
    return 1;
}

static void refill_rack(Game *g, Player *p) {
    while (p->rack_len < RACK_SIZE && draw_tile(g, &p->rack[p->rack_len])) p->rack_len++;
}

static void return_to_bag(Game *g, const Tile *t) {
    Tile back = *t;
    back.id = 0;
    g->bag[g->bag_len++] = back;
}

Game *game_new(const Dict *dict, int vs_ai, uint64_t seed) {
    Game *g = xcalloc(1, sizeof *g);
    g->dict = dict;
    g->rng = seed ? seed : random_seed();
    g->vs_ai = vs_ai ? 1 : 0;
    g->first_move = 1;
    g->next_tile_id = 1;

    for (int l = 0; l < 26; l++) {
        for (int k = 0; k < LETTER_COUNTS[l]; k++) {
            Tile t = { .id = 0, .letter = (char)('A' + l), .points = (unsigned char)LETTER_VALUES[l], .is_blank = 0 };
            g->bag[g->bag_len++] = t;
        }
    }
    for (int k = 0; k < 2; k++) {
        Tile t = { .id = 0, .letter = 0, .points = 0, .is_blank = 1 };
        g->bag[g->bag_len++] = t;
    }
    shuffle_bag(g);

    snprintf(g->players[0].name, sizeof g->players[0].name, "%s", vs_ai ? "Vous" : "Joueur 1");
    snprintf(g->players[1].name, sizeof g->players[1].name, "%s", vs_ai ? "IA" : "Joueur 2");
    g->players[1].is_ai = g->vs_ai;
    refill_rack(g, &g->players[0]);
    refill_rack(g, &g->players[1]);

    g->last_used = time(NULL);
    snprintf(g->message, sizeof g->message,
             "Nouvelle partie ! Le premier mot doit passer par la case centrale. A %s de jouer.",
             g->players[0].name);
    return g;
}

void game_free(Game *g) {
    xfree(g);
}

int game_ai_pending(const Game *g) {
    return !g->game_over && g->players[g->current].is_ai;
}

/* ---------- Mots sur le plateau ---------- */

typedef struct { int r, c; } Pos;
typedef struct { int n; Pos cells[BOARD_SIZE]; } Span;

/* Mot maximal (horizontal ou vertical) passant par (r, c) sur le plateau b. */
static void word_span(const Cell b[BOARD_SIZE][BOARD_SIZE], int r, int c, int horiz, Span *w) {
    int sr = r, sc = c;
    if (horiz) {
        while (sc > 0 && b[r][sc - 1].letter) sc--;
    } else {
        while (sr > 0 && b[sr - 1][c].letter) sr--;
    }
    w->n = 0;
    int cr = sr, cc = sc;
    while (cr < BOARD_SIZE && cc < BOARD_SIZE && b[cr][cc].letter) {
        w->cells[w->n].r = cr;
        w->cells[w->n].c = cc;
        w->n++;
        if (horiz) cc++;
        else cr++;
    }
}

static void span_str(const Cell b[BOARD_SIZE][BOARD_SIZE], const Span *w, char *out) {
    for (int i = 0; i < w->n; i++) out[i] = b[w->cells[i].r][w->cells[i].c].letter;
    out[w->n] = '\0';
}

static int span_has_center(const Span *w) {
    for (int i = 0; i < w->n; i++) {
        if (w->cells[i].r == 7 && w->cells[i].c == 7) return 1;
    }
    return 0;
}

/* ---------- Validation et score d'un coup ---------- */

typedef struct {
    int r, c;
    char letter;
    unsigned char points;
    unsigned char is_blank;
    int tile_id;
} NewTile;

typedef struct {
    int score;
    char main_word[DICT_MAX_WORD + 1];
} Eval;

/*
 * Verifie les regles pour un coup deja pose sur le plateau de travail wb
 * (newmask marque les nouvelles lettres) et calcule le score.
 * Retourne 0 si le coup est valide, -1 sinon (message dans err si err != NULL).
 */
static int evaluate(const Dict *d, const Cell wb[BOARD_SIZE][BOARD_SIZE],
                    const unsigned char newmask[BOARD_SIZE][BOARD_SIZE],
                    const NewTile *nt, int nnew, int first_move,
                    Eval *ev, char *err, size_t errsz) {
    Span words[MAX_NEW + 1];
    int nw = 0;

#define FAIL(...) do { if (err) snprintf(err, errsz, __VA_ARGS__); return -1; } while (0)

    if (nnew < 1) FAIL("%s", "Placez au moins une lettre sur le plateau.");

    if (nnew == 1) {
        Span h, v;
        word_span(wb, nt[0].r, nt[0].c, 1, &h);
        word_span(wb, nt[0].r, nt[0].c, 0, &v);
        if (h.n > 1) words[nw++] = h;
        if (v.n > 1) words[nw++] = v;
        if (nw == 0) {
            FAIL("%s", first_move ? "Le premier mot doit contenir au moins 2 lettres."
                                  : "Cette lettre doit toucher un mot deja present sur le plateau.");
        }
        if (first_move) {
            int ok = 0;
            for (int i = 0; i < nw; i++) if (span_has_center(&words[i])) ok = 1;
            if (!ok) FAIL("%s", "Le premier mot doit passer par la case centrale.");
        }
    } else {
        int same_row = 1, same_col = 1;
        for (int i = 1; i < nnew; i++) {
            if (nt[i].r != nt[0].r) same_row = 0;
            if (nt[i].c != nt[0].c) same_col = 0;
        }
        int horiz;
        if (same_row) horiz = 1;
        else if (same_col) horiz = 0;
        else FAIL("%s", "Les lettres doivent etre alignees sur une seule ligne ou colonne.");

        Span mainw;
        word_span(wb, nt[0].r, nt[0].c, horiz, &mainw);
        for (int i = 0; i < nnew; i++) {
            int found = 0;
            for (int k = 0; k < mainw.n; k++) {
                if (mainw.cells[k].r == nt[i].r && mainw.cells[k].c == nt[i].c) { found = 1; break; }
            }
            if (!found) FAIL("%s", "Les lettres doivent former une ligne continue, sans case vide.");
        }
        words[nw++] = mainw;

        for (int i = 0; i < nnew; i++) {
            Span cross;
            word_span(wb, nt[i].r, nt[i].c, !horiz, &cross);
            if (cross.n > 1) words[nw++] = cross;
        }
        if (first_move && !span_has_center(&mainw)) FAIL("%s", "Le premier mot doit passer par la case centrale.");
    }

    if (!first_move) {
        int touches = 0;
        for (int w = 0; w < nw && !touches; w++) {
            int fresh = 0;
            for (int k = 0; k < words[w].n; k++) {
                if (newmask[words[w].cells[k].r][words[w].cells[k].c]) fresh++;
            }
            if (words[w].n > fresh) touches = 1;
        }
        if (!touches) FAIL("%s", "Votre mot doit toucher une lettre deja posee sur le plateau.");
    }

    for (int w = 0; w < nw; w++) {
        char s[DICT_MAX_WORD + 1];
        span_str(wb, &words[w], s);
        if (!dict_contains(d, s)) FAIL("\"%s\" n'est pas un mot reconnu par le dictionnaire.", s);
    }

    int total = 0;
    for (int w = 0; w < nw; w++) {
        int word_score = 0, word_mult = 1;
        for (int k = 0; k < words[w].n; k++) {
            int r = words[w].cells[k].r, c = words[w].cells[k].c;
            int letter_mult = 1;
            if (newmask[r][c]) {
                switch (PREMIUM_MAP[r][c]) {
                    case 'l': letter_mult = 2; break;
                    case 'L': letter_mult = 3; break;
                    case 'w': word_mult *= 2; break;
                    case 'W': word_mult *= 3; break;
                    default: break;
                }
            }
            word_score += (wb[r][c].is_blank ? 0 : wb[r][c].points) * letter_mult;
        }
        total += word_score * word_mult;
    }
    if (nnew == RACK_SIZE) total += BINGO_BONUS;

    ev->score = total;
    span_str(wb, &words[0], ev->main_word);
    return 0;
#undef FAIL
}

/* ---------- Fin de tour ---------- */

static void end_turn(Game *g, const char *base) {
    g->current = g->current ? 0 : 1;
    const Player *np = &g->players[g->current];
    if (np->is_ai) snprintf(g->message, sizeof g->message, "%s L'IA reflechit...", base);
    else snprintf(g->message, sizeof g->message, "%s A %s de jouer.", base, np->name);
}

static void set_final_message(Game *g, const char *base) {
    snprintf(g->message, sizeof g->message, "%s Partie terminee ! Scores finaux - %s : %d, %s : %d.",
             base, g->players[0].name, g->players[0].score, g->players[1].name, g->players[1].score);
}

/* Fin de coup : soit la partie est finie (sac vide et main vide), soit on passe la main. */
static void finish_move(Game *g, const Player *mover, const char *base) {
    g->scoreless_turns = 0;
    if (g->bag_len == 0 && mover->rack_len == 0) {
        g->game_over = 1;
        set_final_message(g, base);
    } else {
        end_turn(g, base);
    }
}

/* Fin d'un tour sans points (passe ou echange) ; six de suite terminent la partie. */
static void finish_scoreless(Game *g, const char *base) {
    if (++g->scoreless_turns >= MAX_SCORELESS_TURNS) {
        char msg[200];
        snprintf(msg, sizeof msg, "%s Six tours de suite sans points :", base);
        g->game_over = 1;
        set_final_message(g, msg);
    } else {
        end_turn(g, base);
    }
}

#define ERR(...) do { if (err) snprintf(err, errsz, __VA_ARGS__); return -1; } while (0)

/* ---------- Actions du joueur humain ---------- */

int game_play(Game *g, const PlayTile *pt, int n, char *err, size_t errsz) {
    if (g->game_over) ERR("%s", "La partie est terminee.");
    Player *mover = &g->players[g->current];
    if (mover->is_ai) ERR("%s", "Ce n'est pas votre tour.");
    if (n < 1) ERR("%s", "Placez au moins une lettre sur le plateau.");
    if (n > mover->rack_len) ERR("%s", "Vous posez plus de lettres que vous n'en avez.");

    NewTile nt[MAX_NEW];
    int used[RACK_SIZE] = {0};
    Cell wb[BOARD_SIZE][BOARD_SIZE];
    unsigned char newmask[BOARD_SIZE][BOARD_SIZE];
    memcpy(wb, g->board, sizeof wb);
    memset(newmask, 0, sizeof newmask);

    for (int i = 0; i < n; i++) {
        int ti = -1;
        for (int j = 0; j < mover->rack_len; j++) {
            if (mover->rack[j].id == pt[i].id) { ti = j; break; }
        }
        if (ti < 0 || used[ti]) ERR("%s", "Lettre inconnue ou posee deux fois.");
        used[ti] = 1;

        int r = pt[i].row, c = pt[i].col;
        if (r < 0 || r >= BOARD_SIZE || c < 0 || c >= BOARD_SIZE) ERR("%s", "Position invalide.");
        if (wb[r][c].letter) ERR("%s", "Case deja occupee.");

        const Tile *t = &mover->rack[ti];
        char letter = t->letter;
        if (t->is_blank) {
            letter = (char)toupper((unsigned char)pt[i].letter);
            if (letter < 'A' || letter > 'Z') ERR("%s", "Veuillez entrer une seule lettre (A-Z).");
        }
        unsigned char points = t->is_blank ? 0 : t->points;
        wb[r][c].letter = letter;
        wb[r][c].points = points;
        wb[r][c].is_blank = t->is_blank;
        newmask[r][c] = 1;
        nt[i].r = r;
        nt[i].c = c;
        nt[i].letter = letter;
        nt[i].points = points;
        nt[i].is_blank = t->is_blank;
        nt[i].tile_id = t->id;
    }

    Eval ev;
    if (evaluate(g->dict, wb, newmask, nt, n, g->first_move, &ev, err, errsz) != 0) return -1;

    /* Le coup est valide : a partir d'ici on modifie l'etat. */
    memcpy(g->board, wb, sizeof wb);
    int kept = 0;
    for (int j = 0; j < mover->rack_len; j++) {
        if (!used[j]) mover->rack[kept++] = mover->rack[j];
    }
    mover->rack_len = kept;
    mover->score += ev.score;
    g->first_move = 0;
    refill_rack(g, mover);

    char base[160];
    snprintf(base, sizeof base, "Coup valide (%s) ! +%d points.", mover->name, ev.score);
    finish_move(g, mover, base);
    return 0;
}

int game_exchange(Game *g, const int *ids, int n, char *err, size_t errsz) {
    if (g->game_over) ERR("%s", "La partie est terminee.");
    Player *mover = &g->players[g->current];
    if (mover->is_ai) ERR("%s", "Ce n'est pas votre tour.");
    if (n < 1) ERR("%s", "Selectionnez au moins une lettre a echanger.");
    if (n > mover->rack_len) ERR("%s", "Vous echangez plus de lettres que vous n'en avez.");
    if (g->bag_len < n) ERR("%s", "Pas assez de lettres dans le sac pour echanger.");

    int used[RACK_SIZE] = {0};
    for (int i = 0; i < n; i++) {
        int ti = -1;
        for (int j = 0; j < mover->rack_len; j++) {
            if (mover->rack[j].id == ids[i]) { ti = j; break; }
        }
        if (ti < 0 || used[ti]) ERR("%s", "Lettre inconnue ou selectionnee deux fois.");
        used[ti] = 1;
    }

    /* Regle officielle : on pioche d'abord, puis on remet ses lettres dans le sac. */
    Tile drawn[RACK_SIZE];
    for (int i = 0; i < n; i++) draw_tile(g, &drawn[i]);
    int kept = 0;
    for (int j = 0; j < mover->rack_len; j++) {
        if (used[j]) return_to_bag(g, &mover->rack[j]);
        else mover->rack[kept++] = mover->rack[j];
    }
    shuffle_bag(g);
    for (int i = 0; i < n; i++) mover->rack[kept++] = drawn[i];
    mover->rack_len = kept;

    char base[160];
    snprintf(base, sizeof base, "%s echange %d lettre(s).", mover->name, n);
    finish_scoreless(g, base);
    return 0;
}

int game_pass(Game *g, char *err, size_t errsz) {
    if (g->game_over) ERR("%s", "La partie est terminee.");
    const Player *mover = &g->players[g->current];
    if (mover->is_ai) ERR("%s", "Ce n'est pas votre tour.");
    char base[160];
    snprintf(base, sizeof base, "%s passe son tour.", mover->name);
    finish_scoreless(g, base);
    return 0;
}

int game_end(Game *g, char *err, size_t errsz) {
    if (g->game_over) ERR("%s", "La partie est deja terminee.");
    g->game_over = 1;
    set_final_message(g, "Partie terminee.");
    return 0;
}

/* ---------- Recherche de coups (IA et suggestions) ---------- */

typedef struct {
    int score;
    int nnew;
    NewTile nt[MAX_NEW];
    char word[DICT_MAX_WORD + 1];
} Move;

typedef void (*MoveCb)(const Move *m, void *ud);

typedef struct {
    const Dict *d;
    Cell wb[BOARD_SIZE][BOARD_SIZE];                 /* copie du plateau ; les candidats y sont poses puis retires */
    unsigned char newmask[BOARD_SIZE][BOARD_SIZE];
    unsigned char adj[BOARD_SIZE][BOARD_SIZE];       /* case vide touchant une lettre posee */
    const Tile *rack;
    int rack_len;
    int8_t counts[26];                               /* lettres de la main (hors blancs) */
    int blanks;
    uint32_t rack_mask;                              /* lettres distinctes de la main */
    uint32_t avail_mask;                             /* lettres de la main + lettres du plateau */
    int first_move;
    long budget;
    MoveCb cb;
    void *ud;
} Search;

/* Essaie de poser `word` a partir de (r0, c0). overlap_from >= 0 : la premiere lettre du
   plateau reutilisee est a cet index (chaque placement n'est ainsi genere qu'une fois) ;
   overlap_from < 0 : le mot ne doit reutiliser aucune lettre du plateau. */
static void try_candidate(Search *s, const char *word, int wlen, int r0, int c0, int horiz, int overlap_from) {
    if (s->budget <= 0) return;
    s->budget--;

    int dr = horiz ? 0 : 1, dc = horiz ? 1 : 0;
    int er = r0 + dr * (wlen - 1), ec = c0 + dc * (wlen - 1);
    if (r0 < 0 || c0 < 0 || er >= BOARD_SIZE || ec >= BOARD_SIZE) return;

    /* Une lettre collee avant ou apres donnerait un mot plus long : ce cas est traite avec
       ce mot-la, a partir de sa propre position. */
    int pr = r0 - dr, pc = c0 - dc, nr = er + dr, nc = ec + dc;
    if (pr >= 0 && pc >= 0 && s->wb[pr][pc].letter) return;
    if (nr < BOARD_SIZE && nc < BOARD_SIZE && s->wb[nr][nc].letter) return;

    int8_t cnt[26];
    memcpy(cnt, s->counts, sizeof cnt);
    int blanks_left = s->blanks;
    int limit = overlap_from < 0 ? wlen : overlap_from;

    NewTile nt[MAX_NEW];
    int nnew = 0, touches = 0, covers_center = 0;
    for (int k = 0; k < wlen; k++) {
        int r = r0 + dr * k, c = c0 + dc * k;
        char ch = word[k];
        char existing = s->wb[r][c].letter;
        if (existing) {
            if (k < limit || existing != ch) return;
            continue;
        }
        if (nnew >= MAX_NEW) return;
        int li = ch - 'A';
        if (cnt[li] > 0) cnt[li]--;
        else if (blanks_left > 0) blanks_left--;
        else return;
        nt[nnew].r = r;
        nt[nnew].c = c;
        nt[nnew].letter = ch;
        nnew++;
        if (s->adj[r][c]) touches = 1;
        if (r == 7 && c == 7) covers_center = 1;
    }
    if (nnew == 0) return;
    if (s->first_move) {
        if (!covers_center) return;
    } else if (overlap_from < 0 && !touches) {
        return;
    }

    /* Choix des jetons : la lettre exacte d'abord, un blanc seulement si necessaire. */
    int used[RACK_SIZE] = {0};
    for (int i = 0; i < nnew; i++) {
        int pick = -1;
        for (int j = 0; j < s->rack_len; j++) {
            if (!used[j] && !s->rack[j].is_blank && s->rack[j].letter == nt[i].letter) { pick = j; break; }
        }
        if (pick < 0) {
            for (int j = 0; j < s->rack_len; j++) {
                if (!used[j] && s->rack[j].is_blank) { pick = j; break; }
            }
        }
        if (pick < 0) return; /* ne devrait pas arriver : la faisabilite a ete verifiee plus haut */
        used[pick] = 1;
        nt[i].tile_id = s->rack[pick].id;
        nt[i].is_blank = s->rack[pick].is_blank;
        nt[i].points = s->rack[pick].is_blank ? 0 : (unsigned char)LETTER_VALUES[nt[i].letter - 'A'];
    }

    for (int i = 0; i < nnew; i++) {
        Cell *cell = &s->wb[nt[i].r][nt[i].c];
        cell->letter = nt[i].letter;
        cell->points = nt[i].points;
        cell->is_blank = nt[i].is_blank;
        s->newmask[nt[i].r][nt[i].c] = 1;
    }
    Eval ev;
    int ok = evaluate(s->d, s->wb, s->newmask, nt, nnew, s->first_move, &ev, NULL, 0) == 0;
    for (int i = 0; i < nnew; i++) {
        s->wb[nt[i].r][nt[i].c].letter = 0;
        s->newmask[nt[i].r][nt[i].c] = 0;
    }
    if (!ok) return;

    Move m;
    m.score = ev.score;
    m.nnew = nnew;
    memcpy(m.nt, nt, sizeof(NewTile) * (size_t)nnew);
    memcpy(m.word, ev.main_word, sizeof m.word);
    s->cb(&m, s->ud);
}

/* Le mot est-il epelable uniquement avec la main (blancs compris) ? */
static int rack_can_spell(const Search *s, const char *w, int len) {
    int8_t cnt[26];
    memcpy(cnt, s->counts, sizeof cnt);
    int bl = s->blanks;
    for (int i = 0; i < len; i++) {
        int li = w[i] - 'A';
        if (cnt[li] > 0) cnt[li]--;
        else if (bl > 0) bl--;
        else return 0;
    }
    return 1;
}

/* Explore les coups plausibles et appelle cb pour chaque coup valide. Deux generateurs
   complementaires, sans doublon : (1) mots poses entierement depuis la main, (2) mots
   traversant au moins une lettre deja posee (via l'index position -> lettre du dictionnaire). */
static void search_moves(const Game *g, const Tile *rack, int rack_len, MoveCb cb, void *ud) {
    Search s;
    memset(&s, 0, sizeof s);
    s.d = g->dict;
    memcpy(s.wb, g->board, sizeof s.wb);
    s.rack = rack;
    s.rack_len = rack_len;
    s.first_move = g->first_move;
    s.budget = AI_SEARCH_BUDGET;
    s.cb = cb;
    s.ud = ud;

    for (int j = 0; j < rack_len; j++) {
        if (rack[j].is_blank) {
            s.blanks++;
        } else {
            s.counts[rack[j].letter - 'A']++;
            s.rack_mask |= 1u << (rack[j].letter - 'A');
        }
    }
    s.avail_mask = s.rack_mask;
    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int c = 0; c < BOARD_SIZE; c++) {
            if (g->board[r][c].letter) {
                s.avail_mask |= 1u << (g->board[r][c].letter - 'A');
                continue;
            }
            static const int dr[4] = {-1, 1, 0, 0}, dc[4] = {0, 0, -1, 1};
            for (int k = 0; k < 4; k++) {
                int rr = r + dr[k], cc = c + dc[k];
                if (rr >= 0 && rr < BOARD_SIZE && cc >= 0 && cc < BOARD_SIZE && g->board[rr][cc].letter) s.adj[r][c] = 1;
            }
        }
    }

    const Dict *d = s.d;

    /* Generateur 1 : mots entierement poses depuis la main. */
    int maxlen = rack_len < BOARD_SIZE ? rack_len : BOARD_SIZE;
    for (int len = 2; len <= maxlen && s.budget > 0; len++) {
        for (uint32_t k = 0; k < d->len_cnt[len] && s.budget > 0; k++) {
            uint32_t wi = d->len_order[d->len_off[len] + k];
            if (__builtin_popcount(d->masks[wi] & ~s.rack_mask) > s.blanks) continue;
            const char *w = d->words[wi];
            if (!rack_can_spell(&s, w, len)) continue;

            for (int horiz = 1; horiz >= 0; horiz--) {
                if (s.first_move) {
                    int lo = 7 - (len - 1) > 0 ? 7 - (len - 1) : 0;
                    int hi = 7 < BOARD_SIZE - len ? 7 : BOARD_SIZE - len;
                    for (int p = lo; p <= hi; p++) {
                        if (horiz) try_candidate(&s, w, len, 7, p, 1, -1);
                        else try_candidate(&s, w, len, p, 7, 0, -1);
                    }
                } else {
                    int rmax = horiz ? BOARD_SIZE - 1 : BOARD_SIZE - len;
                    int cmax = horiz ? BOARD_SIZE - len : BOARD_SIZE - 1;
                    for (int r = 0; r <= rmax; r++) {
                        for (int c = 0; c <= cmax; c++) try_candidate(&s, w, len, r, c, horiz, -1);
                    }
                }
            }
        }
    }

    /* Generateur 2 : mots traversant une lettre du plateau. */
    if (g->first_move) return;
    for (int r = 0; r < BOARD_SIZE && s.budget > 0; r++) {
        for (int c = 0; c < BOARD_SIZE && s.budget > 0; c++) {
            char letter = g->board[r][c].letter;
            if (!letter) continue;
            int li = letter - 'A';
            for (int horiz = 1; horiz >= 0 && s.budget > 0; horiz--) {
                int anchor = horiz ? c : r;
                for (int i = 0; i <= anchor && s.budget > 0; i++) {
                    int r0 = horiz ? r : r - i, c0 = horiz ? c - i : c;
                    const uint32_t *list = d->pos_pool + d->pos_off[i][li];
                    uint32_t cnt = d->pos_cnt[i][li];
                    for (uint32_t k = 0; k < cnt && s.budget > 0; k++) {
                        uint32_t wi = list[k];
                        int wlen = d->lens[wi];
                        if ((horiz ? c0 : r0) + wlen > BOARD_SIZE) continue;
                        if (__builtin_popcount(d->masks[wi] & ~s.avail_mask) > s.blanks) continue;
                        try_candidate(&s, d->words[wi], wlen, r0, c0, horiz, i);
                    }
                }
            }
        }
    }
}

typedef struct {
    Move best;
    int have;
} BestCtx;

static void best_cb(const Move *m, void *ud) {
    BestCtx *b = ud;
    if (!b->have || m->score > b->best.score) {
        b->best = *m;
        b->have = 1;
    }
}

typedef struct {
    Move moves[SUGGEST_MAX];
    int n;
    int max;
} TopCtx;

/* Garde les `max` meilleurs coups, un seul par mot (a son meilleur placement), tries par score. */
static void top_cb(const Move *m, void *ud) {
    TopCtx *t = ud;
    for (int i = 0; i < t->n; i++) {
        if (strcmp(t->moves[i].word, m->word) == 0) {
            if (m->score <= t->moves[i].score) return;
            t->moves[i] = *m;
            for (; i > 0 && t->moves[i].score > t->moves[i - 1].score; i--) {
                Move tmp = t->moves[i];
                t->moves[i] = t->moves[i - 1];
                t->moves[i - 1] = tmp;
            }
            return;
        }
    }
    int pos;
    if (t->n < t->max) {
        pos = t->n++;
    } else if (m->score > t->moves[t->n - 1].score) {
        pos = t->n - 1;
    } else {
        return;
    }
    t->moves[pos] = *m;
    for (; pos > 0 && t->moves[pos].score > t->moves[pos - 1].score; pos--) {
        Move tmp = t->moves[pos];
        t->moves[pos] = t->moves[pos - 1];
        t->moves[pos - 1] = tmp;
    }
}

int game_suggest(const Game *g, Suggestion *out, int max) {
    if (g->game_over) return -1;
    const Player *p = &g->players[g->current];
    if (p->is_ai) return -1;

    TopCtx top;
    top.n = 0;
    top.max = max < SUGGEST_MAX ? max : SUGGEST_MAX;
    if (top.max <= 0) return 0;
    search_moves(g, p->rack, p->rack_len, top_cb, &top);

    for (int i = 0; i < top.n; i++) {
        const Move *m = &top.moves[i];
        memcpy(out[i].word, m->word, sizeof out[i].word);
        out[i].score = m->score;
        out[i].n = m->nnew;
        for (int k = 0; k < m->nnew; k++) {
            out[i].tiles[k].id = m->nt[k].tile_id;
            out[i].tiles[k].row = m->nt[k].r;
            out[i].tiles[k].col = m->nt[k].c;
            out[i].tiles[k].letter = m->nt[k].letter;
        }
    }
    return top.n;
}

/* ---------- Tour de l'IA ---------- */

int game_ai_turn(Game *g, char *err, size_t errsz) {
    if (g->game_over) ERR("%s", "La partie est terminee.");
    Player *ai = &g->players[g->current];
    if (!ai->is_ai) ERR("%s", "Ce n'est pas le tour de l'IA.");

    BestCtx ctx;
    ctx.have = 0;
    search_moves(g, ai->rack, ai->rack_len, best_cb, &ctx);

    char base[160];
    if (!ctx.have) {
        int n = ai->rack_len < g->bag_len ? ai->rack_len : g->bag_len;
        if (n > 0) {
            Tile drawn[RACK_SIZE];
            for (int i = 0; i < n; i++) draw_tile(g, &drawn[i]);
            int kept = 0;
            for (int j = 0; j < ai->rack_len; j++) {
                if (j < n) return_to_bag(g, &ai->rack[j]);
                else ai->rack[kept++] = ai->rack[j];
            }
            shuffle_bag(g);
            for (int i = 0; i < n; i++) ai->rack[kept++] = drawn[i];
            ai->rack_len = kept;
            snprintf(base, sizeof base, "%s n'a trouve aucun mot jouable et echange %d lettre(s).", ai->name, n);
        } else {
            snprintf(base, sizeof base, "%s n'a trouve aucun mot jouable et passe son tour.", ai->name);
        }
        finish_scoreless(g, base);
        return 0;
    }

    const Move *m = &ctx.best;
    int used[RACK_SIZE] = {0};
    for (int i = 0; i < m->nnew; i++) {
        Cell *cell = &g->board[m->nt[i].r][m->nt[i].c];
        cell->letter = m->nt[i].letter;
        cell->points = m->nt[i].points;
        cell->is_blank = m->nt[i].is_blank;
        for (int j = 0; j < ai->rack_len; j++) {
            if (ai->rack[j].id == m->nt[i].tile_id) { used[j] = 1; break; }
        }
    }
    int kept = 0;
    for (int j = 0; j < ai->rack_len; j++) {
        if (!used[j]) ai->rack[kept++] = ai->rack[j];
    }
    ai->rack_len = kept;
    ai->score += m->score;
    g->first_move = 0;
    refill_rack(g, ai);

    snprintf(base, sizeof base, "%s joue \"%s\" (+%d points).", ai->name, m->word, m->score);
    finish_move(g, ai, base);
    return 0;
}
