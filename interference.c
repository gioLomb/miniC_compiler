#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "interference.h"
#include "regalloc_utils.h"

static inline long tri_idx(int i, int j) {
    if (i < j) { int t = i; i = j; j = t; }
    return (long)i * (i - 1) / 2 + j;
}

static inline int ig_has_edge(const IGraph *g, int i, int j) {
    long idx = tri_idx(i, j);
    return (int)((g->matrix[idx >> 6] >> (idx & 63)) & 1ULL);
}

static void ig_add_edge(IGraph *g, int i, int j) {
    if (i == j || i < 0 || j < 0) return;
    if (ig_has_edge(g, i, j)) return;

    long idx = tri_idx(i, j);
    g->matrix[idx >> 6] |= 1ULL << (idx & 63);

    AdjList *ai = &g->adj[i];
    int_vector_push(ai, j);

    AdjList *aj = &g->adj[j];
    int_vector_push(aj, i);

    g->degree[i]++;
    g->degree[j]++;
}

void ig_free(IGraph *g) {
    if (!g || !g->adj) return;

    for (int i = 0; i < g->n; i++)
        int_vector_free(&g->adj[i]);
}

IGraph ig_build(const MachFunction *f, const BasicBlock *blocks, int nBlocks,
                int nextVreg, const LiveSet *liveAfter, Arena *arena) {
    int totalNodes = nextVreg + PHYS_ALLOCATABLE;

    IGraph g;
    g.n = totalNodes;

    /* --- Matrice triangolare di adiacenza -------------------------------- */
    long   nbits       = (long)totalNodes * (totalNodes - 1) / 2;
    size_t matrixWords = (size_t)((nbits + 63) / 64 + 1);
    g.matrix = arena_alloc(arena, matrixWords * sizeof(uint64_t));
    memset(g.matrix, 0, matrixWords * sizeof(uint64_t));

    /* --- Array di AdjList struct ----------------------------------------- */
    g.adj = arena_alloc(arena, (size_t)totalNodes * sizeof(AdjList));
    for (int i = 0; i < totalNodes; i++)
        int_vector_init(&g.adj[i]);

    /* --- Array interi paralleli ------------------------------------------ */
    g.degree      = arena_alloc(arena, (size_t)totalNodes * sizeof(int));
    g.color       = arena_alloc(arena, (size_t)totalNodes * sizeof(int));
    g.active      = arena_alloc(arena, (size_t)totalNodes * sizeof(bool));
    g.excl        = arena_alloc(arena, (size_t)totalNodes * sizeof(uint32_t));
    g.spillCost   = arena_alloc(arena, (size_t)totalNodes * sizeof(int));
    g.crossesCall = arena_alloc(arena, (size_t)totalNodes * sizeof(char));

    memset(g.degree,      0,    (size_t)totalNodes * sizeof(int));
    memset(g.excl,        0,    (size_t)totalNodes * sizeof(uint32_t));
    memset(g.spillCost,   0,    (size_t)totalNodes * sizeof(int));
    memset(g.crossesCall, 0,    (size_t)totalNodes * sizeof(char));

    /* color = -1: pattern 0xFF valido per int -1 in two's complement */
    memset(g.color,  0xFF, (size_t)totalNodes * sizeof(int));

    /* active = true: sizeof(bool)==1 quindi memset con 1 è corretto */
    memset(g.active, 1,    (size_t)totalNodes * sizeof(bool));

    /* sovrascrive color per registri fisici: indici [nextVreg, nextVreg+PHYS_ALLOCATABLE) */
    for (int p = 0; p < PHYS_ALLOCATABLE; p++)
        g.color[nextVreg + p] = p;

    /* ---- Costruzione archi + spill cost --------------------------------- */
    int tmpArr[LIVENESS_MAX_IDS];
    for (int b = 0; b < nBlocks; b++) {
        for (int i = blocks[b].start; i < blocks[b].end; i++) {
            const MachInstr *in = &f->instrs[i];
            int w = regalloc_spill_weight(in->loopDepth);

            int n;
            instr_uses(in, nextVreg, tmpArr, &n);
            for (int k = 0; k < n; k++)
                if (tmpArr[k] < nextVreg) g.spillCost[tmpArr[k]] += w;

            instr_defs(in, nextVreg, tmpArr, &n);
            for (int k = 0; k < n; k++)
                if (tmpArr[k] < nextVreg) g.spillCost[tmpArr[k]] += w;

            int defs[MAX_EXPLICIT_DEFS], nd, idefs[LIVENESS_MAX_IDS], nid;
            instr_defs(in, nextVreg, defs, &nd);
            instr_implicit_defs(in, nextVreg, idefs, &nid);

            int id;

            for (int d = 0; d < nd; d++) {
                for (LiveSetIter it = LIVESET_ITER(&liveAfter[i]);
                     LIVESET_NEXT(&it, &id); )
                    ig_add_edge(&g, defs[d], id);
            }

            for (int d = 0; d < nid; d++) {
                for (LiveSetIter it = LIVESET_ITER(&liveAfter[i]);
                     LIVESET_NEXT(&it, &id); )
                    ig_add_edge(&g, idefs[d], id);
            }

            if (in->op == MACH_CALL) {
                for (LiveSetIter it = LIVESET_ITER(&liveAfter[i]);
                     LIVESET_NEXT(&it, &id); ) {
                    if (id < nextVreg) {
                        g.excl[id] |= ((1U << PHYS_CALLER_SAVED_COUNT) - 1);
                        g.crossesCall[id] = 1;
                    }
                }
            } else if (in->op == MACH_IDIV || in->op == MACH_CQO) {
                uint32_t mask = (1U << PHYS_RAX) | (1U << PHYS_RDX);
                for (LiveSetIter it = LIVESET_ITER(&liveAfter[i]);
                     LIVESET_NEXT(&it, &id); )
                    if (id < nextVreg) g.excl[id] |= mask;
            } else if (regalloc_is_setcc(in->op)) {
                uint32_t mask = (1U << PHYS_RAX);
                for (LiveSetIter it = LIVESET_ITER(&liveAfter[i]);
                     LIVESET_NEXT(&it, &id); )
                    if (id < nextVreg) g.excl[id] |= mask;
            }
        }
    }

    return g;
}