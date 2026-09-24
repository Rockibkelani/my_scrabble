/*
 * Tests du moteur de jeu et des briques memoire. Lancer : make test
 * (compile avec ASan/UBSan : toute fuite ou erreur memoire fait echouer le test).
 */

#include "server.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

static int passes = 0, failures = 0;

#define CHECK(cond, ...)                                          \
    do {                                                          \
        if (cond) {                                               \
            passes++;                                             \
        } else {                                                  \
            failures++;                                           \
            printf("ECHEC (ligne %d) : ", __LINE__);              \
            printf(__VA_ARGS__);                                  \
            printf("\n");                                         \
        }                                                         \
    } while (0)

static Dict dict;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static Tile mk(int id, char letter, int points, int blank) {
    Tile t;
    t.id = id;
    t.letter = blank ? 0 : letter;
    t.points = (unsigned char)(blank ? 0 : points);
    t.is_blank = (unsigned char)blank;
    return t;
}

static void set_rack(Game *g, int player, const Tile *tiles, int n) {
    for (int i = 0; i < n; i++) g->players[player].rack[i] = tiles[i];
    g->players[player].rack_len = n;
}

static PlayTile pt(int id, int row, int col, char letter) {
    PlayTile p = { id, row, col, letter };
    return p;
}

/* Un coup refuse doit renvoyer -1, le bon message, et laisser la partie strictement inchangee. */
static void expect_refused(Game *g, const PlayTile *tiles, int n, const char *needle, const char *label) {
    Game before = *g;
    char err[200] = "";
    int rc = game_play(g, tiles, n, err, sizeof err);
    CHECK(rc == -1, "%s : le coup aurait du etre refuse", label);
    CHECK(strstr(err, needle) != NULL, "%s : message \"%s\" ne contient pas \"%s\"", label, err, needle);
    CHECK(memcmp(&before, g, sizeof before) == 0, "%s : l'etat a change malgre le refus", label);
}

static int count_board_tiles(const Game *g) {
    int n = 0;
    for (int r = 0; r < BOARD_SIZE; r++)
        for (int c = 0; c < BOARD_SIZE; c++)
            if (g->board[r][c].letter) n++;
    return n;
}

/* Tous les mots de 2 lettres ou plus (horizontaux et verticaux) doivent etre au dictionnaire. */
static int board_words_valid(const Game *g, char *bad, size_t badsz) {
    for (int dir = 0; dir < 2; dir++) {
        for (int a = 0; a < BOARD_SIZE; a++) {
            char word[BOARD_SIZE + 1];
            int len = 0;
            for (int b = 0; b <= BOARD_SIZE; b++) {
                char ch = 0;
                if (b < BOARD_SIZE) ch = dir == 0 ? g->board[a][b].letter : g->board[b][a].letter;
                if (ch) {
                    word[len++] = ch;
                } else {
                    if (len >= 2) {
                        word[len] = '\0';
                        if (!dict_contains(&dict, word)) {
                            snprintf(bad, badsz, "%s", word);
                            return 0;
                        }
                    }
                    len = 0;
                }
            }
        }
    }
    return 1;
}

/* ---------- Dictionnaire ---------- */

static void test_dict(void) {
    CHECK(dict.count > 300000, "dictionnaire trop petit : %zu", dict.count);
    CHECK(dict_contains(&dict, "CHATS") && dict_contains(&dict, "MAISONS") && dict_contains(&dict, "VU"),
          "mots courants absents");
    CHECK(!dict_contains(&dict, "VUW") && !dict_contains(&dict, "XYZZY") && !dict_contains(&dict, "A"),
          "mots invalides acceptes");

    Dict bad;
    CHECK(dict_load(&bad, "data/n_existe_pas.txt") == -1, "fichier absent doit echouer");
    dict_free(&bad);

    /* Fichier non trie, avec doublon, fins de ligne Windows, minuscules et mots hors limites. */
    const char *tmp = "/tmp/test_words_scrabble.txt";
    FILE *f = fopen(tmp, "wb");
    CHECK(f != NULL, "creation du fichier temporaire");
    if (f) {
        fputs("ZEBRE\nAMI\nAMI\r\nchat\nBOUM\nA\nABCDEFGHIJKLMNOP\n", f);
        fclose(f);
        Dict small;
        CHECK(dict_load(&small, tmp) == 0, "chargement du petit dictionnaire");
        CHECK(small.count == 3, "attendu 3 mots (tries, sans doublon), obtenu %zu", small.count);
        CHECK(dict_contains(&small, "AMI") && dict_contains(&small, "BOUM") && dict_contains(&small, "ZEBRE"),
              "mots du petit dictionnaire");
        CHECK(!dict_contains(&small, "CHAT"), "un mot en minuscules ne doit pas etre charge");
        dict_free(&small);
        dict_free(&small); /* double liberation sans danger */
        remove(tmp);
    }
    f = fopen(tmp, "wb");
    if (f) {
        fputs("abc\n12\n", f); /* aucun mot valide */
        fclose(f);
        Dict none;
        CHECK(dict_load(&none, tmp) == -1, "un fichier sans mot valide doit echouer");
        dict_free(&none);
        remove(tmp);
    }
}

/* ---------- Creation d'une partie ---------- */

static void test_new_game(void) {
    Game *g = game_new(&dict, 0, 1234);
    CHECK(g->bag_len == BAG_TOTAL - 2 * RACK_SIZE, "sac initial : %d", g->bag_len);
    CHECK(g->players[0].rack_len == RACK_SIZE && g->players[1].rack_len == RACK_SIZE, "mains initiales");
    CHECK(g->first_move && !g->game_over && g->current == 0, "etat initial");

    int seen[64] = {0}, unique = 1;
    for (int p = 0; p < 2; p++)
        for (int i = 0; i < RACK_SIZE; i++) {
            int id = g->players[p].rack[i].id;
            if (id < 1 || id >= 64 || seen[id]++) unique = 0;
        }
    CHECK(unique, "identifiants de jetons uniques");

    /* Composition du sac complet : on remet tout ensemble et on compte. */
    int letters[26] = {0}, blanks = 0, total = 0;
    for (int i = 0; i < g->bag_len; i++) {
        total++;
        if (g->bag[i].is_blank) blanks++;
        else letters[g->bag[i].letter - 'A']++;
    }
    for (int p = 0; p < 2; p++)
        for (int i = 0; i < RACK_SIZE; i++) {
            total++;
            const Tile *t = &g->players[p].rack[i];
            if (t->is_blank) blanks++;
            else letters[t->letter - 'A']++;
        }
    CHECK(total == BAG_TOTAL && blanks == 2, "102 jetons dont 2 blancs (total %d, blancs %d)", total, blanks);
    CHECK(letters['E' - 'A'] == 15 && letters['A' - 'A'] == 9 && letters['Z' - 'A'] == 1, "distribution des lettres");

    Game *h = game_new(&dict, 0, 1234);
    CHECK(memcmp(g->players[0].rack, h->players[0].rack, sizeof g->players[0].rack) == 0, "meme graine, meme tirage");
    Game *k = game_new(&dict, 0, 99);
    CHECK(memcmp(g->players[0].rack, k->players[0].rack, sizeof g->players[0].rack) != 0, "graines differentes");
    game_free(g);
    game_free(h);
    game_free(k);
}

/* ---------- Regles et score (mains truquees, resultats connus) ---------- */

static void test_rules(void) {
    Game *g = game_new(&dict, 0, 42);
    Tile r0[] = { mk(100, 'V', 4, 0), mk(101, 0, 0, 1) };
    set_rack(g, 0, r0, 2);
    Tile r1[] = { mk(200, 'Z', 10, 0), mk(201, 'Y', 10, 0), mk(202, 'X', 10, 0), mk(203, 'W', 10, 0), mk(204, 0, 0, 1) };
    set_rack(g, 1, r1, 5);

    /* Premier coup : refus avant toute reussite. */
    PlayTile one[] = { pt(100, 7, 7, 0) };
    expect_refused(g, one, 1, "au moins 2 lettres", "premier coup : une seule lettre");
    PlayTile off[] = { pt(100, 0, 0, 0), pt(101, 0, 1, 'U') };
    expect_refused(g, off, 2, "case centrale", "premier coup hors centre");
    PlayTile blank_none[] = { pt(100, 7, 6, 0), pt(101, 7, 7, '1') };
    expect_refused(g, blank_none, 2, "A-Z", "blanc sans lettre valide");
    PlayTile twice[] = { pt(100, 7, 6, 0), pt(100, 7, 7, 0) };
    expect_refused(g, twice, 2, "deux fois", "meme jeton deux fois");
    PlayTile stranger[] = { pt(999, 7, 6, 0), pt(101, 7, 7, 'U') };
    expect_refused(g, stranger, 2, "inconnue", "jeton inconnu");
    PlayTile oob[] = { pt(100, 7, 6, 0), pt(101, 15, 7, 'U') };
    expect_refused(g, oob, 2, "invalide", "hors du plateau");
    PlayTile same_cell[] = { pt(100, 7, 7, 0), pt(101, 7, 7, 'U') };
    expect_refused(g, same_cell, 2, "occupee", "deux lettres sur la meme case");
    expect_refused(g, one, 0, "au moins une lettre", "aucune lettre");
    PlayTile too_many[8];
    for (int i = 0; i < 8; i++) too_many[i] = pt(100, 7, i, 0);
    expect_refused(g, too_many, 8, "plus de lettres", "plus de lettres que la main");
    PlayTile vuw[] = { pt(100, 7, 6, 0), pt(101, 7, 7, 'W') };
    expect_refused(g, vuw, 2, "\"VW\"", "mot inconnu");

    /* VU : V (4 pts) + blanc U (0 pt) sur la case centrale (mot double) = 8. */
    PlayTile vu[] = { pt(100, 7, 6, 0), pt(101, 7, 7, 'u') };
    char err[200];
    int rc = game_play(g, vu, 2, err, sizeof err);
    CHECK(rc == 0, "VU valide : %s", err);
    CHECK(g->players[0].score == 8, "score de VU = 8, obtenu %d", g->players[0].score);
    CHECK(g->board[7][6].letter == 'V' && g->board[7][7].letter == 'U' && g->board[7][7].is_blank, "plateau apres VU");
    CHECK(g->board[7][7].points == 0, "un blanc vaut 0");
    CHECK(g->players[0].rack_len == RACK_SIZE, "main reapprovisionnee : %d", g->players[0].rack_len);
    /* Les deux jetons de la main truquee sont poses : la main est vide, elle repioche 7 lettres. */
    CHECK(g->bag_len == BAG_TOTAL - 2 * RACK_SIZE - 7, "sac apres tirage : %d", g->bag_len);
    CHECK(g->current == 1 && !g->first_move, "tour suivant");
    CHECK(strstr(g->message, "Coup valide") && strstr(g->message, "Joueur 2"), "message : %s", g->message);

    /* Tour du joueur 2 : refus. */
    PlayTile gap[] = { pt(200, 9, 3, 0), pt(201, 9, 5, 0) };
    expect_refused(g, gap, 2, "ligne continue", "trou entre les lettres");
    PlayTile alone[] = { pt(202, 0, 0, 0) };
    expect_refused(g, alone, 1, "toucher", "lettre isolee");
    PlayTile apart[] = { pt(200, 9, 3, 0), pt(201, 9, 4, 0) };
    expect_refused(g, apart, 2, "toucher", "mot qui ne touche rien");
    PlayTile diag[] = { pt(200, 8, 7, 0), pt(201, 9, 9, 0) };
    expect_refused(g, diag, 2, "alignees", "lettres non alignees");
    PlayTile onto[] = { pt(200, 7, 6, 0) };
    expect_refused(g, onto, 1, "occupee", "case deja occupee");
    PlayTile bad_word[] = { pt(203, 7, 8, 0) };
    expect_refused(g, bad_word, 1, "VUW", "mot forme inconnu (VUW)");

    /* Joueur 1 n'a pas la main. */
    char err2[200];
    Game before = *g;
    PlayTile mine[] = { pt(g->players[0].rack[0].id, 3, 3, 'A') };
    // game_play agit toujours pour le joueur courant (joueur 2) : un jeton de l'autre main est inconnu.
    CHECK(game_play(g, mine, 1, err2, sizeof err2) == -1, "jeton de l'adversaire refuse");
    CHECK(memcmp(&before, g, sizeof before) == 0, "etat inchange");

    /* Le joueur 2 joue "VUE" ? Non : on verifie plutot un vrai mot croisant VU : "US" vertical sous U. */
    Tile r1b[] = { mk(210, 'S', 1, 0), mk(211, 'E', 1, 0) };
    set_rack(g, 1, r1b, 2);
    PlayTile us[] = { pt(210, 8, 7, 0) };
    rc = game_play(g, us, 1, err, sizeof err);
    CHECK(rc == 0, "US vertical valide : %s", err);
    CHECK(g->players[1].score == 1, "US : U (blanc, 0) + S (1) sur case normale = 1, obtenu %d", g->players[1].score);

    /* Fin de partie manuelle. */
    CHECK(game_end(g, err, sizeof err) == 0 && g->game_over, "fin de partie");
    CHECK(game_end(g, err, sizeof err) == -1, "fin de partie deja terminee");
    PlayTile late[] = { pt(211, 9, 7, 0) };
    CHECK(game_play(g, late, 1, err, sizeof err) == -1, "plus de coup apres la fin");
    CHECK(game_pass(g, err, sizeof err) == -1, "plus de passe apres la fin");
    game_free(g);

    /* Bonus de 50 points : sept lettres posees d'un coup ("ETUDIER" ne passe pas sans blanc,
       on verifie donc la regle sur un mot de 7 lettres ayant un vrai score). */
    g = game_new(&dict, 0, 7);
    Tile bingo[] = { mk(1, 'M', 2, 0), mk(2, 'A', 1, 0), mk(3, 'I', 1, 0), mk(4, 'S', 1, 0), mk(5, 'O', 1, 0),
                     mk(6, 'N', 1, 0), mk(7, 'S', 1, 0) };
    set_rack(g, 0, bingo, 7);
    PlayTile mai[7];
    const int ids[7] = { 1, 2, 3, 4, 5, 6, 7 };
    for (int i = 0; i < 7; i++) mai[i] = pt(ids[i], 7, 4 + i, 0); /* MAISONS de (7,4) a (7,10) */
    rc = game_play(g, mai, 7, err, sizeof err);
    CHECK(rc == 0, "MAISONS valide : %s", err);
    /* M2+A1+I1+S1 (7,7 = mot double, lettre normale)+O1+N1+S1, (7,3) hors mot ; (7,4)=rien,(7,7)=MD,(7,11) hors. */
    /* (7,4)M (7,5)A (7,6)I (7,7)S=MD (7,8)O (7,9)N (7,10)S -> 2+1+1+1+1+1+1 = 8, x2 = 16, +50 = 66 */
    CHECK(g->players[0].score == 66, "MAISONS = 66 avec le bonus de 50, obtenu %d", g->players[0].score);
    game_free(g);
}

/* ---------- Echange, passe, fin ---------- */

static void test_exchange_pass(void) {
    Game *g = game_new(&dict, 0, 5);
    char err[200];
    int bag_before = g->bag_len;
    int ids[2] = { g->players[0].rack[0].id, g->players[0].rack[3].id };
    CHECK(game_exchange(g, ids, 2, err, sizeof err) == 0, "echange : %s", err);
    CHECK(g->bag_len == bag_before, "l'echange conserve la taille du sac");
    CHECK(g->players[0].rack_len == RACK_SIZE, "main complete apres echange");
    CHECK(g->current == 1, "l'echange termine le tour");
    CHECK(strstr(g->message, "echange 2"), "message : %s", g->message);

    Game before = *g;
    int dup[2] = { g->players[1].rack[0].id, g->players[1].rack[0].id };
    CHECK(game_exchange(g, dup, 2, err, sizeof err) == -1, "echange avec doublon refuse");
    int unknown[1] = { 9999 };
    CHECK(game_exchange(g, unknown, 1, err, sizeof err) == -1, "echange avec jeton inconnu refuse");
    CHECK(game_exchange(g, unknown, 0, err, sizeof err) == -1, "echange vide refuse");
    CHECK(memcmp(&before, g, sizeof before) == 0, "etat inchange apres les refus");

    g->bag_len = 1;
    int two[2] = { g->players[1].rack[0].id, g->players[1].rack[1].id };
    Game small_bag = *g;
    CHECK(game_exchange(g, two, 2, err, sizeof err) == -1, "pas assez de lettres dans le sac");
    CHECK(memcmp(&small_bag, g, sizeof small_bag) == 0, "etat inchange (sac trop petit)");

    CHECK(game_pass(g, err, sizeof err) == 0 && g->current == 0, "passe le tour");
    CHECK(g->first_move, "passer ne change pas first_move");
    Suggestion sug[SUGGEST_MAX];
    CHECK(game_suggest(g, sug, SUGGEST_MAX) >= 0, "suggestions pour un humain");
    game_end(g, err, sizeof err);
    CHECK(game_suggest(g, sug, SUGGEST_MAX) == -1, "pas de suggestion apres la fin");
    game_free(g);

    g = game_new(&dict, 1, 5);
    CHECK(game_suggest(g, sug, SUGGEST_MAX) >= 0, "suggestions pour l'humain (mode IA)");
    CHECK(game_ai_turn(g, err, sizeof err) == -1, "l'IA ne joue pas a la place de l'humain");
    CHECK(game_pass(g, err, sizeof err) == 0, "l'humain passe");
    CHECK(game_ai_pending(g), "c'est a l'IA");
    CHECK(game_pass(g, err, sizeof err) == -1, "l'humain ne peut pas passer pour l'IA");
    CHECK(game_suggest(g, sug, SUGGEST_MAX) == -1, "pas de suggestion pour l'IA");
    CHECK(game_ai_turn(g, err, sizeof err) == 0, "tour de l'IA : %s", err);
    CHECK(g->current == 0 || g->game_over, "la main revient a l'humain");
    game_free(g);

    /* Six tours de suite sans points (passes ou echanges) terminent la partie. */
    g = game_new(&dict, 0, 11);
    for (int i = 0; i < 5; i++) {
        CHECK(game_pass(g, err, sizeof err) == 0 && !g->game_over, "passe %d : la partie continue", i + 1);
    }
    int one[1] = { g->players[g->current].rack[0].id };
    CHECK(game_exchange(g, one, 1, err, sizeof err) == 0, "6e tour sans points (echange)");
    CHECK(g->game_over && strstr(g->message, "Six tours"), "fin apres 6 tours sans points : %s", g->message);
    game_free(g);
}

/* ---------- Coherence suggestions <-> validation ---------- */

static void test_suggestions_match_play(void) {
    int checked = 0;
    for (uint64_t seed = 1; seed <= 25; seed++) {
        Game *g = game_new(&dict, 0, seed);
        for (int turn = 0; turn < 10 && !g->game_over; turn++) {
            Suggestion sug[SUGGEST_MAX];
            int n = game_suggest(g, sug, SUGGEST_MAX);
            CHECK(n >= 0, "suggestions disponibles");
            char err[200];
            int cur = g->current;
            for (int i = 0; i < n; i++) {
                Game copy = *g;
                int rc = game_play(&copy, sug[i].tiles, sug[i].n, err, sizeof err);
                CHECK(rc == 0, "suggestion \"%s\" refusee au jeu : %s", sug[i].word, err);
                if (rc == 0) {
                    CHECK(copy.players[cur].score - g->players[cur].score == sug[i].score,
                          "score de \"%s\" : suggere %d, joue %d", sug[i].word, sug[i].score,
                          copy.players[cur].score - g->players[cur].score);
                    checked++;
                }
                if (i > 0) CHECK(sug[i - 1].score >= sug[i].score, "suggestions triees par score");
                for (int j = 0; j < i; j++) CHECK(strcmp(sug[i].word, sug[j].word) != 0, "mots uniques");
            }
            if (n > 0) game_play(g, sug[0].tiles, sug[0].n, err, sizeof err);
            else game_pass(g, err, sizeof err);
        }
        game_free(g);
    }
    CHECK(checked > 100, "assez de suggestions verifiees (%d)", checked);
    printf("  %d suggestions verifiees contre le moteur de validation\n", checked);
}

/* ---------- Parties completes IA contre IA, avec invariants ---------- */

static void test_ai_vs_ai(void) {
    double worst = 0, total = 0;
    int turns_all = 0, plays = 0, finished = 0;
    for (uint64_t seed = 1; seed <= 3; seed++) {
        Game *g = game_new(&dict, 1, seed * 7919);
        g->players[0].is_ai = 1; /* deux IA : la partie se joue toute seule */
        snprintf(g->players[0].name, sizeof g->players[0].name, "IA 1");
        char err[200], bad[32];
        int turns = 0;
        while (!g->game_over && turns < 300) {
            int before_score = g->players[0].score + g->players[1].score;
            double t0 = now_ms();
            int rc = game_ai_turn(g, err, sizeof err);
            double dt = now_ms() - t0;
            if (dt > worst) worst = dt;
            total += dt;
            turns++;
            turns_all++;
            CHECK(rc == 0, "tour d'IA : %s", err);
            if (rc != 0) break;
            if (g->players[0].score + g->players[1].score > before_score) plays++;

            int tiles = count_board_tiles(g);
            int in_racks = g->players[0].rack_len + g->players[1].rack_len;
            CHECK(tiles + g->bag_len + in_racks == BAG_TOTAL, "conservation des 102 jetons (%d+%d+%d)", tiles, g->bag_len, in_racks);
            CHECK(board_words_valid(g, bad, sizeof bad), "mot invalide sur le plateau : %s", bad);
            CHECK(g->players[0].rack_len <= RACK_SIZE && g->players[1].rack_len <= RACK_SIZE, "taille des mains");
            if (g->bag_len > 0 && !g->game_over) {
                CHECK(g->players[0].rack_len == RACK_SIZE && g->players[1].rack_len == RACK_SIZE, "mains completes tant que le sac n'est pas vide");
            }
            CHECK(g->players[0].score >= 0 && g->players[1].score >= 0, "scores positifs");
        }
        if (g->game_over) finished++;
        else game_end(g, err, sizeof err);
        game_free(g);
    }
    printf("  %d tours d'IA (%d avec un mot pose), %d parties terminees naturellement\n", turns_all, plays, finished);
    printf("  duree par tour d'IA sous ASan : moyenne %.1f ms, pire %.1f ms\n", total / turns_all, worst);
    CHECK(plays > 30, "les IA doivent poser des mots (%d)", plays);
    CHECK(worst < 8000, "un tour d'IA ne doit pas depasser 8 s meme sous ASan (%.0f ms)", worst);
}

/* ---------- Briques : json, url, html, dynbuf, allocateur ---------- */

static void test_helpers(void) {
    int ids[4], n = 0;
    CHECK(json_get_int_array("{\"tile_ids\": [1, 22 ,333]}", "tile_ids", ids, 4, &n) == 1 && n == 3 && ids[2] == 333, "tableau d'entiers");
    CHECK(json_get_int_array("{}", "tile_ids", ids, 4, &n) == 0, "cle absente");
    CHECK(json_get_int_array("{\"tile_ids\":[1,2,3,4,5]}", "tile_ids", ids, 4, &n) == -1, "trop d'entiers");
    CHECK(json_get_int_array("{\"tile_ids\":[1,x]}", "tile_ids", ids, 4, &n) == -1, "entier mal forme");
    CHECK(json_get_int_array("{\"tile_ids\":[1,2", "tile_ids", ids, 4, &n) == -1, "tableau non termine");
    CHECK(json_get_int_array("{\"tile_ids\":[99999999999]}", "tile_ids", ids, 4, &n) == -1, "entier hors plage");

    long v = 0;
    CHECK(json_get_int("{\"row\": -3}", "row", &v) == 1 && v == -3, "entier negatif");
    CHECK(json_get_int("{\"row\": \"a\"}", "row", &v) == 0, "chaine au lieu d'un entier");

    const char *body = "{\"tiles\":[{\"id\":1,\"row\":7,\"col\":6},{\"id\":2,\"row\":7,\"col\":7,\"letter\":\"U\"}]}";
    const char *cur = json_array_begin(body, "tiles");
    char obj[64];
    int count = 0, rc;
    while ((rc = json_array_next_object(&cur, obj, sizeof obj)) == 1) count++;
    CHECK(rc == 0 && count == 2, "iteration sur 2 objets (rc=%d, n=%d)", rc, count);
    cur = json_array_begin("{\"tiles\":[{\"id\":1,\"row\":7,\"col\":6,\"pad\":\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\"}]}", "tiles");
    CHECK(json_array_next_object(&cur, obj, sizeof obj) == -1, "objet trop grand pour le tampon");
    cur = json_array_begin("{\"tiles\":[{\"id\":1", "tiles");
    CHECK(json_array_next_object(&cur, obj, sizeof obj) == -1, "objet non termine");
    CHECK(json_array_begin("{\"tiles\":3}", "tiles") == NULL, "tiles n'est pas un tableau");

    char small[8];
    url_decode(small, sizeof small, "ab%20cd+efghijklmnop");
    CHECK(strlen(small) == 7 && strncmp(small, "ab cd e", 7) == 0, "url_decode tronque proprement : \"%s\"", small);
    char tiny[1];
    url_decode(tiny, sizeof tiny, "abc");
    CHECK(tiny[0] == '\0', "url_decode avec un tampon de 1 octet");
    char pct[8];
    url_decode(pct, sizeof pct, "%4");
    CHECK(strcmp(pct, "%4") == 0, "pourcentage incomplet laisse tel quel");

    DynBuf b;
    dynbuf_init(&b);
    html_escape_append(&b, "<b>\"Tom & 'Jerry'\"</b>");
    CHECK(strcmp(b.data, "&lt;b&gt;&quot;Tom &amp; &#39;Jerry&#39;&quot;&lt;/b&gt;") == 0, "echappement HTML : %s", b.data);
    dynbuf_free(&b);

    dynbuf_init(&b);
    for (int i = 0; i < 2000; i++) dynbuf_printf(&b, "%d,", i);
    CHECK(b.len > 8000 && b.data[b.len] == '\0' && strncmp(b.data, "0,1,2,", 6) == 0, "dynbuf_printf sur de gros volumes (%zu)", b.len);
    dynbuf_free(&b);
    dynbuf_free(&b); /* double liberation sans danger */
}

static void test_allocator(void) {
    size_t base = mem_live_allocs();
    void *a = xmalloc(100);
    void *b = xcalloc(10, 10);
    CHECK(mem_live_allocs() == base + 2, "deux allocations comptees");
    unsigned char *z = b;
    int zero = 1;
    for (int i = 0; i < 100; i++) if (z[i]) zero = 0;
    CHECK(zero, "xcalloc met a zero");
    a = xrealloc(a, 5000);
    CHECK(mem_live_allocs() == base + 2, "realloc ne change pas le nombre de blocs");
    a = xrealloc(a, 10);
    void *c = xrealloc(NULL, 32);
    CHECK(mem_live_allocs() == base + 3, "realloc(NULL) alloue");
    xfree(a);
    xfree(b);
    xfree(c);
    xfree(NULL);
    CHECK(mem_live_allocs() == base, "tout est libere");

    /* Creation/destruction massive de parties : aucune fuite. */
    for (int i = 0; i < 2000; i++) game_free(game_new(&dict, i & 1, (uint64_t)i + 1));
    CHECK(mem_live_allocs() == base, "2000 parties creees puis liberees");
}

int main(void) {
    printf("Chargement du dictionnaire...\n");
    if (dict_load(&dict, SCRABBLE_WORDS_FILE) != 0) {
        printf("Impossible de charger %s (lancer depuis la racine du projet)\n", SCRABBLE_WORDS_FILE);
        return 2;
    }
    printf("  %zu mots\n", dict.count);

    printf("Dictionnaire\n");           test_dict();
    printf("Creation de partie\n");     test_new_game();
    printf("Regles et score\n");        test_rules();
    printf("Echange, passe, fin\n");    test_exchange_pass();
    printf("Suggestions\n");            test_suggestions_match_play();
    printf("Parties IA contre IA\n");   test_ai_vs_ai();
    printf("Briques de base\n");        test_helpers();
    printf("Allocateur\n");             test_allocator();

    dict_free(&dict);
    int leaked = mem_report(stdout);
    CHECK(mem_live_allocs() == 0, "aucune allocation restante a la fin");

    printf("\n%d verifications reussies, %d echec(s)\n", passes, failures);
    return (failures || leaked) ? 1 : 0;
}
