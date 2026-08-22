#include <stdlib.h>
#include <stdint.h>
#include "ra_coalesce.h"

/* ---- inline edge query (same trick as interference.c) ---- */
static inline long tri_idx(int i, int j) {
    // canonical ordering: larger id is always the row index
    if (i < j) { int t = i; i = j; j = t; }
    return (long)i * (i - 1) / 2 + j;
}
static inline int edge_exists(const IGraph *g, int i, int j) {
    // guard against degenerate/self pairs before touching the bit matrix
    if (i < 0 || j < 0 || i == j) return 0;
    long idx = tri_idx(i, j);
    // word index (idx >> 6) selects the uint64_t, (idx & 63) the bit within it
    return (int)((g->matrix[idx >> 6] >> (idx & 63)) & 1ULL);
}

/* ---- list helpers ---- */
static void ml_push(MoveList *ml, int u, int v) {
    // classic doubling growth strategy
    if (ml->count == ml->cap) {
        ml->cap = ml->cap ? ml->cap * 2 : 16;
        ml->pairs = realloc(ml->pairs, (size_t)ml->cap * sizeof(MovePair));
    }
    ml->pairs[ml->count++] = (MovePair){ u, v };
}

/* ---------------------------------------------------------------
 * ra_collect_moves
 *
 * Scans every MACH_MOV vreg<->vreg and vreg<->phys instruction.
 * A pair (u,v) is recorded only if u and v do NOT interfere
 * (otherwise they could never share a color anyway).
 * Physical registers use id = nextVreg + physReg, consistent
 * with IGraph's node layout (physicals in [nextVreg, nextVreg+PHYS_ALLOCATABLE)).
 * --------------------------------------------------------------- */
MoveList ra_collect_moves(const MachFunction *f, const IGraph *g, int nextVreg) {
    MoveList ml = { NULL, 0, 0 };

    for (int i = 0; i < f->count; i++) {
        const MachInstr *in = &f->instrs[i];
        if (in->op != MACH_MOV) continue;

        // resolve numeric ids of dst and src1
        int u = -1, v = -1;

        if (in->dst.kind == MO_VREG)       u = in->dst.vregId;
        else if (in->dst.kind == MO_PHYS)  u = nextVreg + in->dst.physReg;

        if (in->src1.kind == MO_VREG)      v = in->src1.vregId;
        else if (in->src1.kind == MO_PHYS) v = nextVreg + in->src1.physReg;

        // both operands must be identifiable, and at least one must be a
        // vreg (a phys<->phys MOV is never touched by the register allocator)
        if (u < 0 || v < 0) continue;
        if (u >= nextVreg + PHYS_ALLOCATABLE || v >= nextVreg + PHYS_ALLOCATABLE) continue;
        int both_phys = (u >= nextVreg && v >= nextVreg);
        if (both_phys) continue;

        // skip if they interfere: they can never get the same color
        if (edge_exists(g, u, v)) continue;

        ml_push(&ml, u, v);
    }

    return ml;
}

void movelist_free(MoveList *ml) {
    free(ml->pairs);
    ml->pairs = NULL;
    ml->count = ml->cap = 0;
}