#include <stdlib.h>
#include <string.h>
#include "interference.h"
#include "regalloc_utils.h"

long tri_idx(int i, int j) {
    if (i < j) { int t = i; i = j; j = t; }
    return (long)i * (i - 1) / 2 + j;
}

int ig_has_edge(const IGraph *g, int i, int j) {
    long idx = tri_idx(i, j);
    return (int)((g->matrix[idx >> 6] >> (idx & 63)) & 1ULL);
}

void ig_add_edge(IGraph *g, int i, int j) {
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

void ig_free(IGraph *g) {
    for (int i = 0; i < g->n; i++)
        free(g->adj[i].data);
    free(g->adj);
    free(g->matrix);
    free(g->degree);
    free(g->color);
    free(g->active);
    free(g->excl);
    free(g->spillCost);
    free(g->crossesCall);
}

/* Riceve BasicBlock* (ex RBlock*): layout identico, tipo unificato */
IGraph ig_build(const MachFunction *f, const BasicBlock *blocks, int nBlocks,
                int nextVreg, const LiveSet *liveAfter) {
    int totalNodes = nextVreg + PHYS_ALLOCATABLE;
    IGraph g;
    g.n = totalNodes;
    long nbits = (long)totalNodes * (totalNodes - 1) / 2;
    g.matrix      = calloc((size_t)((nbits + 63) / 64 + 1), sizeof(uint64_t));
    g.adj         = calloc((size_t)totalNodes, sizeof(AdjList));
    g.degree      = calloc((size_t)totalNodes, sizeof(int));
    g.color       = malloc((size_t)totalNodes * sizeof(int));
    g.active      = malloc((size_t)totalNodes * sizeof(int));
    g.excl        = calloc((size_t)totalNodes, sizeof(uint32_t));
    g.spillCost   = calloc((size_t)totalNodes, sizeof(int));
    g.crossesCall = calloc((size_t)totalNodes, 1);

    for (int i = 0; i < totalNodes; i++) {
        g.color[i]  = -1;
        g.active[i] = 1;
    }
    for (int p = 0; p < PHYS_ALLOCATABLE; p++)
        g.color[nextVreg + p] = p;

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

            for (int d = 0; d < nd; d++) {
                LIVESET_FOREACH(&liveAfter[i], id)
                    ig_add_edge(&g, defs[d], id);
                LIVESET_FOREACH_END
            }
            for (int d = 0; d < nid; d++) {
                LIVESET_FOREACH(&liveAfter[i], id)
                    ig_add_edge(&g, idefs[d], id);
                LIVESET_FOREACH_END
            }

            if (in->op == MACH_CALL) {
                LIVESET_FOREACH(&liveAfter[i], id)
                    if (id < nextVreg) {
                        g.excl[id] |= ((1U << PHYS_CALLER_SAVED_COUNT) - 1);
                        g.crossesCall[id] = 1;
                    }
                LIVESET_FOREACH_END
            } else if (in->op == MACH_IDIV || in->op == MACH_CQO) {
                uint32_t mask = (1U << PHYS_RAX) | (1U << PHYS_RDX);
                LIVESET_FOREACH(&liveAfter[i], id)
                    if (id < nextVreg) g.excl[id] |= mask;
                LIVESET_FOREACH_END
            } else if (regalloc_is_setcc(in->op)) {
                uint32_t mask = (1U << PHYS_RAX);
                LIVESET_FOREACH(&liveAfter[i], id)
                    if (id < nextVreg) g.excl[id] |= mask;
                LIVESET_FOREACH_END
            }
        }
    }
    return g;
}