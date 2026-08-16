#ifndef BITSET_H
#define BITSET_H

#include <stdint.h>
#include <string.h>
#include "arena.h"

/*
 * BitSet — bitset generico su arena.
 *
 * Tipo primitivo riusabile in contesti diversi:
 *   - liveness.h lo reexporta come LiveSet (bit = variabile viva)
 *   - loop.h lo usa direttamente come BitSet (bit = blocco dominatore)
 *
 * Tutti i bit-word sono allocati dall'arena del chiamante;
 * nessuna free individuale necessaria.
 */

typedef struct {
    uint64_t *bits;
    int       words;  /* numero di uint64_t allocati */
} BitSet;

/* ---- Costruzione ---------------------------------------------------- */

/* Alloca e azzera un BitSet di 'words' parole dall'arena. */
static inline BitSet bitset_new(Arena *arena, int words) {
    BitSet s;
    s.words = words;
    s.bits  = arena_alloc(arena, (size_t)words * sizeof(uint64_t));
    memset(s.bits, 0, (size_t)words * sizeof(uint64_t));
    return s;
}

/* ---- Operazioni sui singoli bit ------------------------------------- */

static inline void bitset_set(BitSet *s, int id) {
    s->bits[id >> 6] |= 1ULL << (id & 63);
}

static inline void bitset_clr(BitSet *s, int id) {
    s->bits[id >> 6] &= ~(1ULL << (id & 63));
}

static inline int bitset_test(const BitSet *s, int id) {
    return (int)((s->bits[id >> 6] >> (id & 63)) & 1ULL);
}

/* ---- Operazioni sull'intero set ------------------------------------- */

static inline void bitset_clear(BitSet *s) {
    memset(s->bits, 0, (size_t)s->words * sizeof(uint64_t));
}

static inline void bitset_copy(BitSet *dst, const BitSet *src) {
    memcpy(dst->bits, src->bits, (size_t)src->words * sizeof(uint64_t));
}

static inline int bitset_equal(const BitSet *a, const BitSet *b) {
    for (int i = 0; i < a->words; i++)
        if (a->bits[i] != b->bits[i]) return 0;
    return 1;
}

/* dst |= src */
static inline void bitset_or(BitSet *dst, const BitSet *src) {
    for (int i = 0; i < dst->words; i++) dst->bits[i] |= src->bits[i];
}

/* dst &= src */
static inline void bitset_and(BitSet *dst, const BitSet *src) {
    for (int i = 0; i < dst->words; i++) dst->bits[i] &= src->bits[i];
}

/* dst = a & ~b  (differenza insiemistica) */
static inline void bitset_diff(BitSet *dst, const BitSet *a, const BitSet *b) {
    for (int i = 0; i < dst->words; i++) dst->bits[i] = a->bits[i] & ~b->bits[i];
}

/* dst = a | b */
static inline void bitset_union_into(BitSet *dst,
                                     const BitSet *a, const BitSet *b) {
    for (int i = 0; i < dst->words; i++) dst->bits[i] = a->bits[i] | b->bits[i];
}

/* ---- Iteratore sui bit settati -------------------------------------- */

/*
 * Uso:
 *   int id;
 *   for (BitSetIter it = BITSET_ITER(&s); BITSET_NEXT(&it, &id); )
 *       ... usa id ...
 */
typedef struct {
    const BitSet *s;
    int           word;
    uint64_t      bits;
} BitSetIter;

#define BITSET_ITER(s) \
    { (s), 0, ((s)->words > 0 ? (s)->bits[0] : 0ULL) }

static inline int bitset_iter_next(BitSetIter *it, int *out_id) {
    while (it->bits == 0) {
        it->word++;
        if (it->word >= it->s->words) return 0;
        it->bits = it->s->bits[it->word];
    }
    int b    = __builtin_ctzll(it->bits);
    *out_id  = (it->word << 6) + b;
    it->bits &= it->bits - 1;
    return 1;
}

#define BITSET_NEXT(it, out_id) bitset_iter_next((it), (out_id))

#endif /* BITSET_H */