#include <stdlib.h>
#include <string.h>
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

/* adj.data: unica allocazione fuori dall'arena, cresce per archi aggiunti */
static void ig_add_edge(IGraph *g, int i, int j) {
    if (i == j || i < 0 || j < 0) return;
    if (ig_has_edge(g, i, j)) return;

    long idx = tri_idx(i, j);
    g->matrix[idx >> 6] |= 1ULL << (idx & 63);

    AdjList *ai = &g->adj[i];
    if (ai->len == ai->cap) {
        ai->cap  = ai->cap ? ai->cap * 2 : 4;
        ai->data = realloc(ai->data, (size_t)ai->cap * sizeof(int));
    }
    ai->data[ai->len++] = j;

    AdjList *aj = &g->adj[j];
    if (aj->len == aj->cap) {
        aj->cap  = aj->cap ? aj->cap * 2 : 4;
        aj->data = realloc(aj->data, (size_t)aj->cap * sizeof(int));
    }
    aj->data[aj->len++] = i;

    g->degree[i]++;
    g->degree[j]++;
}

/* Libera solo adj.data; tutto il resto e' nell'arena del caller */
void ig_free(IGraph *g) {
    for (int i = 0; i < g->n; i++)
        free(g->adj[i].data);
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

    /* --- Array di AdjList struct (adj[i].data resta NULL, malloc separato) */
    g.adj = arena_alloc(arena, (size_t)totalNodes * sizeof(AdjList));
    memset(g.adj, 0, (size_t)totalNodes * sizeof(AdjList));

    /* --- Array interi paralleli ------------------------------------------ */
    g.degree      = arena_alloc(arena, (size_t)totalNodes * sizeof(int));
    g.color       = arena_alloc(arena, (size_t)totalNodes * sizeof(int));
    g.active      = arena_alloc(arena, (size_t)totalNodes * sizeof(int));
    g.excl        = arena_alloc(arena, (size_t)totalNodes * sizeof(uint32_t));
    g.spillCost   = arena_alloc(arena, (size_t)totalNodes * sizeof(int));
    g.crossesCall = arena_alloc(arena, (size_t)totalNodes * sizeof(char));

    memset(g.degree,      0, (size_t)totalNodes * sizeof(int));
    memset(g.excl,        0, (size_t)totalNodes * sizeof(uint32_t));
    memset(g.spillCost,   0, (size_t)totalNodes * sizeof(int));
    memset(g.crossesCall, 0, (size_t)totalNodes * sizeof(char));

    for (int i = 0; i < totalNodes; i++) {
        g.color[i]  = -1;
        g.active[i] =  1;
    }
    for (int p = 0; p < PHYS_ALLOCATABLE; p++)
        g.color[nextVreg + p] = p;

    /* ---- Costruzione archi + spill cost --------------------------------- */
    int tmpArr[16];
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

            int defs[8], nd, idefs[16], nid;
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