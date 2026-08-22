#include <stdlib.h>
#include <stdint.h>
#include "ra_coalesce.h"

/* =========================================================================
 * Triangular-matrix edge query — mirrors interference.c
 * ========================================================================= */

/**
 * Map the unordered pair (i, j) to its flat lower-triangular index.
 * Canonical form enforced: larger id becomes the row (i > j after swap).
 */
static inline long tri_idx(int i, int j) {
    if (i < j) { int t = i; i = j; j = t; }
    // standard lower-triangular formula: row i starts at i*(i-1)/2
    return (long)i * (i - 1) / 2 + j;
}

/** Return non-zero if an interference edge exists between nodes @p i and @p j. */
static inline int edge_exists(const IGraph *g, int i, int j) {
    // reject degenerate or self-pairs before touching the bit matrix
    if (i < 0 || j < 0 || i == j) return 0;
    long idx = tri_idx(i, j);
    // word index (idx >> 6) selects the uint64_t, (idx & 63) the bit within it
    return (int)((g->matrix[idx >> 6] >> (idx & 63)) & 1ULL);
}

/* =========================================================================
 * PartnerList helpers
 * ========================================================================= */

/** Append the pair (u, v) to @p pl, doubling capacity when needed. */
static void pl_push(PartnerList *pl, int u, int v) {
    // start at 16 to amortise early reallocations on small functions
    if (pl->count == pl->cap) {
        pl->cap   = pl->cap ? pl->cap * 2 : 16;
        pl->pairs = realloc(pl->pairs, (size_t)pl->cap * sizeof(PartnerPair));
    }
    pl->pairs[pl->count++] = (PartnerPair){ u, v };
}

/* =========================================================================
 * Public API
 * ========================================================================= */

PartnerList ra_collect_partners(const MachFunction *f, const IGraph *g, int nextVreg)
{
    PartnerList pl = { NULL, 0, 0 };

    for (int i = 0; i < f->count; i++) {
        const MachInstr *in = &f->instrs[i];
        if (in->op != MACH_MOV) continue;

        // resolve numeric node ids for the destination and source operands
        int u = -1, v = -1;

        if      (in->dst.kind  == MO_VREG) u = in->dst.vregId;
        else if (in->dst.kind  == MO_PHYS) u = nextVreg + in->dst.physReg;

        if      (in->src1.kind == MO_VREG) v = in->src1.vregId;
        else if (in->src1.kind == MO_PHYS) v = nextVreg + in->src1.physReg;

        // both operands must resolve to valid node ids
        if (u < 0 || v < 0) continue;

        // guard against out-of-range ids (e.g. PHYS_AL which is beyond PHYS_ALLOCATABLE)
        if (u >= nextVreg + PHYS_ALLOCATABLE || v >= nextVreg + PHYS_ALLOCATABLE) continue;

        // phys↔phys MOVs are never touched by the register allocator
        if (u >= nextVreg && v >= nextVreg) continue;

        // only record pairs that do NOT already interfere: interfering nodes can
        // never share a color, so a hint for them would be silently ignored anyway
        if (edge_exists(g, u, v)) continue;

        pl_push(&pl, u, v);
    }

    return pl;
}

void partnerlist_free(PartnerList *pl) {
    free(pl->pairs);
    // reset all fields so a double-free attempt produces a clean no-op
    pl->pairs = NULL;
    pl->count = pl->cap = 0;
}