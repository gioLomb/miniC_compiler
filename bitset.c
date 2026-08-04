#include "bitset.h"

RSet rset_new(int n) {
    RSet s;
    s.words = (n + 63) / 64;
    s.bits  = calloc((size_t)s.words, sizeof(uint64_t));
    return s;
}

void rset_free(RSet *s) {
    free(s->bits);
    s->bits = NULL;
}

void rset_clear(RSet *s) {
    memset(s->bits, 0, (size_t)s->words * sizeof(uint64_t));
}

void rset_set(RSet *s, int id) {
    s->bits[id >> 6] |= 1ULL << (id & 63);
}

void rset_clr(RSet *s, int id) {
    s->bits[id >> 6] &= ~(1ULL << (id & 63));
}

int rset_test(const RSet *s, int id) {
    return (int)((s->bits[id >> 6] >> (id & 63)) & 1ULL);
}

void rset_copy(RSet *dst, const RSet *src) {
    memcpy(dst->bits, src->bits, (size_t)src->words * sizeof(uint64_t));
}

void rset_union(RSet *dst, const RSet *src) {
    for (int i = 0; i < dst->words; i++)
        dst->bits[i] |= src->bits[i];
}

void rset_diff(RSet *dst, const RSet *a, const RSet *b) {
    for (int i = 0; i < dst->words; i++)
        dst->bits[i] = a->bits[i] & ~b->bits[i];
}

int rset_equal(const RSet *a, const RSet *b) {
    for (int i = 0; i < a->words; i++)
        if (a->bits[i] != b->bits[i]) return 0;
    return 1;
}