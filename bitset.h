#ifndef BITSET_H
#define BITSET_H

#include <stdint.h>
#include <string.h>
#include "arena.h"

/**
 * @file bitset.h
 * @brief Generic arena-backed bit-set.
 *
 * Reusable primitive type shared across unrelated domains:
 *   - liveness.h re-exports it as LiveSet (bit = variable id).
 *   - loop.h uses it directly as BitSet (bit = dominator block index).
 *
 * All bit-words are allocated from the caller's Arena; no individual
 * free is required (lifetime is tied to the arena).
 */

/**
 * @brief Fixed-capacity bit-set backed by an array of 64-bit words.
 *
 * Capacity (in bits) is words * 64. Bit ids are expected to be in
 * range [0, words*64) — no bounds checking is performed by the
 * per-bit operations below.
 */
typedef struct {
    uint64_t *bits;   /**< Arena-allocated array of bit-words. */
    int       words;  /**< Number of uint64_t words allocated.  */
} BitSet;

/* ---- Construction ---------------------------------------------------- */

/**
 * @brief Allocate and zero-initialise a BitSet of @p words words.
 *
 * @param arena  Arena the bit-word array is allocated from.
 * @param words  Number of 64-bit words to allocate (capacity = words*64 bits).
 * @return       A zero-initialised BitSet ready for use.
 */
static inline BitSet bitset_new(Arena *arena, int words) {
    BitSet s;
    s.words = words;
    s.bits  = arena_alloc(arena, (size_t)words * sizeof(uint64_t));
    memset(s.bits, 0, (size_t)words * sizeof(uint64_t));
    return s;
}

/* ---- Single-bit operations -------------------------------------------- */

/**
 * @brief Set bit @p id to 1.
 * @param s   Target bit-set.
 * @param id  Bit index; caller must ensure id < s->words * 64.
 */
static inline void bitset_set(BitSet *s, int id) {
    s->bits[id >> 6] |= 1ULL << (id & 63);
}

/**
 * @brief Clear bit @p id to 0.
 * @param s   Target bit-set.
 * @param id  Bit index; caller must ensure id < s->words * 64.
 */
static inline void bitset_clr(BitSet *s, int id) {
    s->bits[id >> 6] &= ~(1ULL << (id & 63));
}

/**
 * @brief Test whether bit @p id is set.
 * @param s   Bit-set to query.
 * @param id  Bit index; caller must ensure id < s->words * 64.
 * @return    1 if the bit is set, 0 otherwise.
 */
static inline int bitset_test(const BitSet *s, int id) {
    return (int)((s->bits[id >> 6] >> (id & 63)) & 1ULL);
}

/* ---- Whole-set operations ---------------------------------------------- */

/**
 * @brief Clear every bit in @p s (set all words to 0).
 * @param s  Bit-set to clear.
 */
static inline void bitset_clear(BitSet *s) {
    memset(s->bits, 0, (size_t)s->words * sizeof(uint64_t));
}

/**
 * @brief Copy all bits from @p src into @p dst.
 *
 * @param dst  Destination bit-set; must have at least src->words words.
 * @param src  Source bit-set.
 */
static inline void bitset_copy(BitSet *dst, const BitSet *src) {
    memcpy(dst->bits, src->bits, (size_t)src->words * sizeof(uint64_t));
}

/**
 * @brief Compare two bit-sets for equality.
 *
 * @param a  First bit-set.
 * @param b  Second bit-set; must have the same word count as @p a.
 * @return   1 if all words are equal, 0 otherwise.
 */
static inline int bitset_equal(const BitSet *a, const BitSet *b) {
    for (int i = 0; i < a->words; i++)
        if (a->bits[i] != b->bits[i]) return 0;
    return 1;
}

/**
 * @brief In-place union: dst |= src.
 * @param dst  Bit-set updated in place.
 * @param src  Bit-set ORed into @p dst; must have at least dst->words words.
 */
static inline void bitset_or(BitSet *dst, const BitSet *src) {
    for (int i = 0; i < dst->words; i++) dst->bits[i] |= src->bits[i];
}

/**
 * @brief In-place intersection: dst &= src.
 * @param dst  Bit-set updated in place.
 * @param src  Bit-set ANDed into @p dst; must have at least dst->words words.
 */
static inline void bitset_and(BitSet *dst, const BitSet *src) {
    for (int i = 0; i < dst->words; i++) dst->bits[i] &= src->bits[i];
}

/**
 * @brief Set-difference: dst = a & ~b.
 *
 * @param dst  Destination bit-set (may alias @p a).
 * @param a    Minuend set.
 * @param b    Subtrahend set; bits set here are removed from @p a.
 */
static inline void bitset_diff(BitSet *dst, const BitSet *a, const BitSet *b) {
    for (int i = 0; i < dst->words; i++) dst->bits[i] = a->bits[i] & ~b->bits[i];
}

/**
 * @brief Union into a fresh destination: dst = a | b.
 *
 * @param dst  Destination bit-set (may alias @p a or @p b).
 * @param a    First operand.
 * @param b    Second operand.
 */
static inline void bitset_union_into(BitSet *dst,
                                     const BitSet *a, const BitSet *b) {
    for (int i = 0; i < dst->words; i++) dst->bits[i] = a->bits[i] | b->bits[i];
}

/* ---- Iterator over set bits -------------------------------------------- */

/**
 * @brief Iterator state for walking the set bits of a BitSet.
 */
typedef struct {
    const BitSet *s;     /**< Bit-set being iterated.                  */
    int           word;  /**< Index of the current word.               */
    uint64_t      bits;  /**< Remaining unconsumed bits of current word. */
} BitSetIter;

/**
 * @brief Initialise a BitSetIter over @p s.
 * @param s  Pointer to the BitSet to iterate.
 */
#define BITSET_ITER(s) \
    { (s), 0, ((s)->words > 0 ? (s)->bits[0] : 0ULL) }

/**
 * @brief Advance the iterator and retrieve the next set bit id.
 *
 * Skips over empty words automatically; clears the lowest set bit of the
 * current word on each call so successive calls return increasing ids.
 *
 * @param it       Iterator previously initialised with BITSET_ITER.
 * @param out_id   Receives the next set bit's index when the call succeeds.
 * @return         1 if a bit was found and written to out_id; 0 once
 *                 all words have been exhausted.
 */
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

/**
 * @brief Convenience wrapper around bitset_iter_next() for loop conditions.
 * @param it       Iterator previously initialised with BITSET_ITER.
 * @param out_id   Receives the next set bit's index when the call succeeds.
 * @return         Non-zero while bits remain; 0 when iteration is done.
 */
#define BITSET_NEXT(it, out_id) bitset_iter_next((it), (out_id))

#endif /* BITSET_H */