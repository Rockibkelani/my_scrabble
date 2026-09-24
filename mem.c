#include "server.h"

#include <stdlib.h>
#include <string.h>

/*
 * Chaque bloc est precede d'un en-tete (taille + marqueur). Les compteurs sont
 * atomiques car les workers allouent en parallele.
 */
typedef union {
    struct {
        size_t size;
        size_t magic;
    } h;
    max_align_t align;
} MemHdr;

#define MEM_MAGIC ((size_t)0xA110CA7EDULL)
#define MEM_DEAD  ((size_t)0xDEADF4EEULL)

static size_t g_allocs = 0;
static size_t g_frees = 0;
static size_t g_live_bytes = 0;

static void mem_fatal(const char *msg) {
    fprintf(stderr, "mem: %s\n", msg);
    abort();
}

void *xmalloc(size_t n) {
    if (n > SIZE_MAX - sizeof(MemHdr)) mem_fatal("taille demandee excessive");
    MemHdr *h = malloc(sizeof(MemHdr) + n);
    if (!h) mem_fatal("memoire insuffisante");
    h->h.size = n;
    h->h.magic = MEM_MAGIC;
    __atomic_fetch_add(&g_allocs, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_live_bytes, n, __ATOMIC_RELAXED);
    return h + 1;
}

void *xcalloc(size_t nmemb, size_t size) {
    size_t total;
    if (__builtin_mul_overflow(nmemb, size, &total)) mem_fatal("taille demandee excessive");
    void *p = xmalloc(total);
    memset(p, 0, total);
    return p;
}

void *xrealloc(void *p, size_t n) {
    if (!p) return xmalloc(n);
    MemHdr *h = (MemHdr *)p - 1;
    if (h->h.magic != MEM_MAGIC) mem_fatal("xrealloc sur un pointeur invalide ou deja libere");
    if (n > SIZE_MAX - sizeof(MemHdr)) mem_fatal("taille demandee excessive");
    size_t old = h->h.size;
    MemHdr *nh = realloc(h, sizeof(MemHdr) + n);
    if (!nh) mem_fatal("memoire insuffisante");
    nh->h.size = n;
    __atomic_fetch_add(&g_live_bytes, n - old, __ATOMIC_RELAXED); /* wrap voulu si n < old */
    return nh + 1;
}

void xfree(void *p) {
    if (!p) return;
    MemHdr *h = (MemHdr *)p - 1;
    if (h->h.magic != MEM_MAGIC) mem_fatal("xfree sur un pointeur invalide ou double liberation");
    size_t size = h->h.size;
    h->h.magic = MEM_DEAD;
    __atomic_fetch_add(&g_frees, 1, __ATOMIC_RELAXED);
    __atomic_fetch_sub(&g_live_bytes, size, __ATOMIC_RELAXED);
    free(h);
}

size_t mem_live_allocs(void) {
    size_t a = __atomic_load_n(&g_allocs, __ATOMIC_RELAXED);
    size_t f = __atomic_load_n(&g_frees, __ATOMIC_RELAXED);
    return a - f;
}

int mem_report(FILE *out) {
    size_t a = __atomic_load_n(&g_allocs, __ATOMIC_RELAXED);
    size_t f = __atomic_load_n(&g_frees, __ATOMIC_RELAXED);
    size_t bytes = __atomic_load_n(&g_live_bytes, __ATOMIC_RELAXED);
    fprintf(out, "Memoire : %zu allocations, %zu liberations, %zu bloc(s) non libere(s) (%zu octets).\n",
            a, f, a - f, bytes);
    return (a != f || bytes != 0) ? 1 : 0;
}
