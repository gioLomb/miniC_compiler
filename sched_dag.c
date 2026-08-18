#include <string.h>
#include "sched_dag.h"
#include "sched_utils.h"

/* =========================================================================
 * SparseMap
 * ========================================================================= */

void smap_init(SparseMap *m, int cap, Arena *arena) {
    m->sparse = arena_alloc(arena, (size_t)cap * sizeof(int));
    m->dense  = arena_alloc(arena, (size_t)cap * sizeof(int));
    m->val    = arena_alloc(arena, (size_t)cap * sizeof(int));
    m->n   = 0;
    m->cap = cap;
}

int smap_get(const SparseMap *m, int k) {
    if ((unsigned)k >= (unsigned)m->cap) return -1;
    unsigned pos = (unsigned)m->sparse[k];
    if (pos >= (unsigned)m->n || m->dense[pos] != k) return -1;
    return m->val[pos];
}

void smap_set(SparseMap *m, int k, int v) {
    if ((unsigned)k >= (unsigned)m->cap) return;
    unsigned pos = (unsigned)m->sparse[k];
    if (pos < (unsigned)m->n && m->dense[pos] == k) {
        m->val[pos] = v;
        return;
    }
    m->sparse[k]   = m->n;
    m->dense[m->n] = k;
    m->val[m->n]   = v;
    m->n++;
}

/* =========================================================================
 * DAG edge
 * ========================================================================= */

static void dag_add_edge(DAGNode *nodes, int from, int to, Arena *arena) {
    if (from == to) return;
    for (SuccNode *s = nodes[from].succs; s; s = s->next)
        if (s->to == to) return;

    SuccNode *sn      = arena_alloc(arena, sizeof(SuccNode));
    sn->to            = to;
    sn->next          = nodes[from].succs;
    nodes[from].succs = sn;
    nodes[from].nSuccs++;
    nodes[to].predCount++;
}

/* =========================================================================
 * DAG Construction
 * ========================================================================= */

void build_dag(const MachFunction *f, int start, int end, DAGNode *nodes,
               Arena *arena) {
    int n = end - start;
    int universe = f->nextVreg + PHYS_ALLOCATABLE;
    int cap = universe + n;

    int *currentName = arena_alloc(arena, (size_t)universe * sizeof(int));
    for (int r = 0; r < universe; r++) currentName[r] = r;
    int nextFresh = universe;

    SparseMap smap;
    smap_init(&smap, cap, arena);

    /* Inizializza nodi (come già presente) */
    for (int i = 0; i < n; i++) {
        nodes[i] = (DAGNode){
            .instrIdx = start + i,
            .latency  = sched_latency_of(f->instrs[start + i].op),
            .height   = sched_latency_of(f->instrs[start + i].op),
        };
    }

    /* Macro‑fusion (invariato) */
    for (int i = 0; i + 1 < n; i++) {
        if (sched_is_cmp_or_test(f->instrs[start + i].op) &&
            sched_is_jcc(f->instrs[start + i + 1].op))
            nodes[i].pinnedForFusion = 1;
    }

    int lastSideEffect = -1;
    int lastMemoryOp   = -1;   // <-- NUOVA VARIABILE

    for (int j = 0; j < n; j++) {
        const MachInstr *inj = &f->instrs[start + j];

        /* ---- RAW: use dipende dall'ultima definizione (rinominata) ---- */
        int uses[5], nuses;
        sched_uses(inj, f->nextVreg, uses, &nuses);
        for (int u = 0; u < nuses; u++) {
            int r   = uses[u];
            int ren = (r >= 0 && r < universe) ? currentName[r] : r;
            int dep = smap_get(&smap, ren);
            if (dep >= 0) dag_add_edge(nodes, dep, j, arena);
        }

        /* ---- Serializzazione effetti collaterali (CALL, IDIV, ecc.) ---- */
        if (sched_has_side_effect(inj->op)) {
            if (lastSideEffect >= 0)
                dag_add_edge(nodes, lastSideEffect, j, arena);
            lastSideEffect = j;
        }

        /* ---- NUOVO: Serializzazione delle operazioni di memoria ---- */
        /* LOAD e STORE devono rimanere nell'ordine originale tra loro     */
        if (inj->op == MACH_LOAD || inj->op == MACH_STORE) {
            if (lastMemoryOp >= 0)
                dag_add_edge(nodes, lastMemoryOp, j, arena);
            lastMemoryOp = j;
        }

        /* ---- WAW/WAR: rinomina la definizione ---- */
        int d = sched_def(inj, f->nextVreg);
        if (d >= 0 && d < universe) {
            int oldRenamed = currentName[d];
            int prevDef    = smap_get(&smap, oldRenamed);
            if (prevDef >= 0) dag_add_edge(nodes, prevDef, j, arena);

            int fresh      = nextFresh++;
            currentName[d] = fresh;
            smap_set(&smap, fresh, j);
        }
    }

    /* Backward pass per l'altezza (invariato) */
    for (int i = n - 1; i >= 0; i--) {
        int maxH = 0;
        for (SuccNode *s = nodes[i].succs; s; s = s->next)
            if (nodes[s->to].height > maxH) maxH = nodes[s->to].height;
        nodes[i].height = nodes[i].latency + maxH;
    }
}