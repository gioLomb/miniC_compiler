#include "bucket.h"
#include "arena.h"
#include <string.h>

/* Arena globale privata al modulo bucket */
static Arena *s_bucket_arena = NULL;

Buckets buckets_create(int nextVreg, int k) {
    /* Se per qualche motivo l'arena esiste ancora, la distruggiamo
       per garantire uno stato pulito (non dovrebbe accadere se
       buckets_free viene chiamata correttamente) */
    if (s_bucket_arena) {
        arena_destroy(s_bucket_arena);
        s_bucket_arena = NULL;
    }

    s_bucket_arena = arena_create(0);

    Buckets b;
    b.k        = k;
    b.nonempty = 0;

    int n = (nextVreg > 0) ? nextVreg : 1;

    b.head     = arena_alloc(s_bucket_arena, (size_t)k * sizeof(int));
    memset(b.head, -1, (size_t)k * sizeof(int));

    b.bnext    = arena_alloc(s_bucket_arena, (size_t)n * sizeof(int));
    b.bprev    = arena_alloc(s_bucket_arena, (size_t)n * sizeof(int));
    b.inBucket = arena_alloc(s_bucket_arena, (size_t)n * sizeof(int));
    memset(b.inBucket, 0, (size_t)n * sizeof(int));

    return b;
}

void buckets_free(Buckets *b) {
    (void)b; /* parametro mantenuto per coerenza con l'uso esterno */
    if (s_bucket_arena) {
        arena_destroy(s_bucket_arena);
        s_bucket_arena = NULL;
    }
}

void bucket_insert(Buckets *b, int v, int d) {
    b->bprev[v]  = -1;
    b->bnext[v]  = b->head[d];
    if (b->head[d] >= 0)
        b->bprev[b->head[d]] = v;
    b->head[d]   = v;
    b->inBucket[v] = 1;
    b->nonempty |= (1u << d);
}

void bucket_remove(Buckets *b, int v, int d) {
    if (b->bprev[v] >= 0)
        b->bnext[b->bprev[v]] = b->bnext[v];
    else
        b->head[d] = b->bnext[v];

    if (b->bnext[v] >= 0)
        b->bprev[b->bnext[v]] = b->bprev[v];

    b->inBucket[v] = 0;
    if (b->head[d] < 0)
        b->nonempty &= ~(1u << d);
}

int bucket_pop_any_low(Buckets *b, int *outD) {
    if (!b->nonempty) return -1;
    *outD = __builtin_ctz(b->nonempty);
    return b->head[*outD];
}