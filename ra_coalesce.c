#include <stdlib.h>
#include <stdint.h>
#include "ra_coalesce.h"

/* ---- edge query inline (stesso trucco di interference.c) ---- */
static inline long tri_idx(int i, int j) {
    if (i < j) { int t = i; i = j; j = t; }
    return (long)i * (i - 1) / 2 + j;
}
static inline int edge_exists(const IGraph *g, int i, int j) {
    if (i < 0 || j < 0 || i == j) return 0;
    long idx = tri_idx(i, j);
    return (int)((g->matrix[idx >> 6] >> (idx & 63)) & 1ULL);
}

/* ---- helpers lista ---- */
static void ml_push(MoveList *ml, int u, int v) {
    if (ml->count == ml->cap) {
        ml->cap = ml->cap ? ml->cap * 2 : 16;
        ml->pairs = realloc(ml->pairs, (size_t)ml->cap * sizeof(MovePair));
    }
    ml->pairs[ml->count++] = (MovePair){ u, v };
}

/* ---------------------------------------------------------------
 * ra_collect_moves
 *
 * Scansiona tutte le istruzioni MACH_MOV vreg<->vreg e vreg<->phys.
 * Una coppia (u,v) viene registrata solo se u e v NON interferiscono
 * (altrimenti non potrebbero comunque ricevere lo stesso colore).
 * I fisici sono rappresentati con id = nextVreg + physReg, coerente
 * con il layout di IGraph (nodi [nextVreg, nextVreg+PHYS_ALLOCATABLE)).
 * --------------------------------------------------------------- */
MoveList ra_collect_moves(const MachFunction *f, const IGraph *g, int nextVreg) {
    MoveList ml = { NULL, 0, 0 };

    for (int i = 0; i < f->count; i++) {
        const MachInstr *in = &f->instrs[i];
        if (in->op != MACH_MOV) continue;

        /* Determina id numerici di dst e src1. */
        int u = -1, v = -1;

        if (in->dst.kind == MO_VREG)       u = in->dst.vregId;
        else if (in->dst.kind == MO_PHYS)  u = nextVreg + in->dst.physReg;

        if (in->src1.kind == MO_VREG)      v = in->src1.vregId;
        else if (in->src1.kind == MO_PHYS) v = nextVreg + in->src1.physReg;

        /* Entrambi devono essere identificabili e almeno uno deve essere vreg
         * (se entrambi fisici il regalloc non li tocca comunque). */
        if (u < 0 || v < 0) continue;
        if (u >= nextVreg + PHYS_ALLOCATABLE || v >= nextVreg + PHYS_ALLOCATABLE) continue;
        int both_phys = (u >= nextVreg && v >= nextVreg);
        if (both_phys) continue;

        /* Salta se interferiscono: non possono ricevere stesso colore. */
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
