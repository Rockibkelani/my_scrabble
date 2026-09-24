#include "server.h"

#include <stdlib.h>
#include <string.h>

#define DICT_MAX_FILE (64L * 1024 * 1024)

static int cmp_words(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

void dict_free(Dict *d) {
    xfree(d->pool);
    xfree(d->words);
    xfree(d->lens);
    xfree(d->masks);
    xfree(d->pos_pool);
    xfree(d->len_order);
    memset(d, 0, sizeof *d);
}

int dict_load(Dict *d, const char *path) {
    memset(d, 0, sizeof *d);

    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz <= 0 || sz > DICT_MAX_FILE || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }

    d->pool = xmalloc((size_t)sz + 1);
    size_t rd = fread(d->pool, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) { dict_free(d); return -1; }
    d->pool[sz] = '\0';

    /* Une entree par ligne : on remplace les fins de ligne par des NUL et on garde
       uniquement les mots valides (A-Z, 2 a DICT_MAX_WORD lettres). */
    size_t max_words = 1;
    for (long i = 0; i < sz; i++) if (d->pool[i] == '\n') max_words++;
    d->words = xmalloc(max_words * sizeof(*d->words));

    char *p = d->pool;
    char *end = d->pool + sz;
    size_t n = 0;
    while (p < end) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        char *line_end = nl ? nl : end;
        char *next = nl ? nl + 1 : end;
        if (line_end > p && line_end[-1] == '\r') line_end--;
        *line_end = '\0';

        size_t len = (size_t)(line_end - p);
        int ok = len >= 2 && len <= DICT_MAX_WORD;
        for (size_t i = 0; ok && i < len; i++) {
            if (p[i] < 'A' || p[i] > 'Z') ok = 0;
        }
        if (ok) d->words[n++] = p;
        p = next;
    }
    if (n == 0) { dict_free(d); return -1; }

    /* La recherche dichotomique exige un tableau trie et sans doublon. */
    int sorted = 1;
    for (size_t i = 1; i < n; i++) {
        if (strcmp(d->words[i - 1], d->words[i]) >= 0) { sorted = 0; break; }
    }
    if (!sorted) {
        qsort(d->words, n, sizeof(*d->words), cmp_words);
        size_t m = 1;
        for (size_t i = 1; i < n; i++) {
            if (strcmp(d->words[m - 1], d->words[i]) != 0) d->words[m++] = d->words[i];
        }
        n = m;
    }
    d->count = n;

    d->lens = xmalloc(n);
    d->masks = xmalloc(n * sizeof(*d->masks));

    static const uint32_t zero_cnt[DICT_MAX_WORD][26] = {{0}};
    uint32_t cnt[DICT_MAX_WORD][26];
    memcpy(cnt, zero_cnt, sizeof cnt);
    uint32_t len_cnt[DICT_MAX_WORD + 2] = {0};
    size_t total = 0;

    for (size_t i = 0; i < n; i++) {
        const char *w = d->words[i];
        size_t len = strlen(w);
        uint32_t mask = 0;
        for (size_t k = 0; k < len; k++) {
            int l = w[k] - 'A';
            cnt[k][l]++;
            mask |= 1u << l;
        }
        d->lens[i] = (uint8_t)len;
        d->masks[i] = mask;
        len_cnt[len]++;
        total += len;
    }

    /* Index par (position, lettre) : liste des mots ayant cette lettre a cette position. */
    d->pos_pool = xmalloc(total * sizeof(*d->pos_pool));
    uint32_t off = 0;
    for (int k = 0; k < DICT_MAX_WORD; k++) {
        for (int l = 0; l < 26; l++) {
            d->pos_off[k][l] = off;
            d->pos_cnt[k][l] = cnt[k][l];
            off += cnt[k][l];
        }
    }
    uint32_t fill[DICT_MAX_WORD][26];
    memcpy(fill, zero_cnt, sizeof fill);
    for (size_t i = 0; i < n; i++) {
        const char *w = d->words[i];
        for (size_t k = 0; k < d->lens[i]; k++) {
            int l = w[k] - 'A';
            d->pos_pool[d->pos_off[k][l] + fill[k][l]++] = (uint32_t)i;
        }
    }

    /* Index par longueur. */
    d->len_order = xmalloc(n * sizeof(*d->len_order));
    uint32_t loff = 0;
    for (int len = 0; len < DICT_MAX_WORD + 2; len++) {
        d->len_off[len] = loff;
        d->len_cnt[len] = len_cnt[len];
        loff += len_cnt[len];
    }
    uint32_t lfill[DICT_MAX_WORD + 2] = {0};
    for (size_t i = 0; i < n; i++) {
        uint8_t len = d->lens[i];
        d->len_order[d->len_off[len] + lfill[len]++] = (uint32_t)i;
    }
    return 0;
}

int dict_contains(const Dict *d, const char *word) {
    size_t lo = 0, hi = d->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(word, d->words[mid]);
        if (c == 0) return 1;
        if (c < 0) hi = mid;
        else lo = mid + 1;
    }
    return 0;
}
