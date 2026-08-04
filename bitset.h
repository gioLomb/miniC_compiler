#ifndef BITSET_H
#define BITSET_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t *bits;
    int       words;
} RSet;

RSet  rset_new(int n);
void  rset_free(RSet *s);
void  rset_clear(RSet *s);
void  rset_set(RSet *s, int id);
void  rset_clr(RSet *s, int id);
int   rset_test(const RSet *s, int id);
void  rset_copy(RSet *dst, const RSet *src);
void  rset_union(RSet *dst, const RSet *src);
void  rset_diff(RSet *dst, const RSet *a, const RSet *b);
int   rset_equal(const RSet *a, const RSet *b);

#define RSET_FOREACH(s, idvar) \
    for (int _w = 0; _w < (s)->words; _w++) { \
        uint64_t _bits = (s)->bits[_w]; \
        while (_bits) { \
            int _b = __builtin_ctzll(_bits); \
            int idvar = (_w << 6) + _b; \
            _bits &= (_bits - 1);

#define RSET_FOREACH_END } }

#endif