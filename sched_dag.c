#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "sched_dag.h"
#include "sched_utils.h"
#include "arena.h"

/* =========================================================================
 * Arena interna del modulo.
 *
 * Una sola arena per l'intera durata del programma; viene RESETTATA
 * (non distrutta/ricreata) ad ogni build_dag. Questo riusa i blocchi
 * già allocati senza nessun malloc/free tra blocchi consecutivi.
 *
 * Lifecycle: sched_dag_arena_init() → N × build_dag() → sched_dag_arena_fini().
 * Non è thread-safe (compilatore single-threaded).
 * ========================================================================= */
static Arena *s_arena = NULL;

void sched_dag_arena_init(void) {
    if (s_arena) {
        fprintf(stderr, "sched_dag: arena_init chiamata due volte\n");
        return;
    }
    s_arena = arena_create(0);
}

void sched_dag_arena_fini(void) {
    arena_destroy(s_arena);
    s_arena = NULL;
}

/* =========================================================================
 * SparseMap — alloca nell'arena interna.
 * ========================================================================= */

void smap_init(SparseMap *m, int cap) {
    m->sparse = arena_alloc(s_arena, (size_t)cap * sizeof(int));
    m->dense  = arena_alloc(s_arena, (size_t)cap * sizeof(int));
    m->val    = arena_alloc(s_arena, (size_t)cap * sizeof(int));
    m->n = 0; m->cap = cap;
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
    if (pos < (unsigned)m->n && m->dense[pos] == k) { m->val[pos] = v; return; }
    m->sparse[k]   = m->n;
    m->dense[m->n] = k;
    m->val[m->n]   = v;
    m->n++;
}

/* =========================================================================
 * MaxHeap
 * ========================================================================= */

void heap_push(MaxHeap *h, int v, const DAGNode *nodes) {
    int i = h->size++;
    h->data[i] = v;
    while (i > 0) {
        int p = (i - 1) >> 1;
        if (nodes[h->data[p]].height >= nodes[h->data[i]].height) break;
        int t = h->data[p]; h->data[p] = h->data[i]; h->data[i] = t;
        i = p;
    }
}

int heap_pop(MaxHeap *h, const DAGNode *nodes) {
    int top    = h->data[0];
    h->data[0] = h->data[--h->size];
    for (int i = 0;;) {
        int l = 2*i+1, r = 2*i+2, b = i;
        if (l < h->size && nodes[h->data[l]].height > nodes[h->data[b]].height) b = l;
        if (r < h->size && nodes[h->data[r]].height > nodes[h->data[b]].height) b = r;
        if (b == i) break;
        int t = h->data[b]; h->data[b] = h->data[i]; h->data[i] = t;
        i = b;
    }
    return top;
}

/* =========================================================================
 * build_dag — helpers interni
 * ========================================================================= */

static void dag_add_edge(DAGNode *nodes, int from, int to) {
    if (from == to) return;
    for (SuccNode *s = nodes[from].succs; s; s = s->next)
        if (s->to == to) return;
    SuccNode *sn      = arena_alloc(s_arena, sizeof(SuccNode));
    sn->to            = to;
    sn->next          = nodes[from].succs;
    nodes[from].succs = sn;
    nodes[from].nSuccs++;
    nodes[to].predCount++;
}

/* =========================================================================
 * build_dag
 *
 * Resetta l'arena interna all'inizio: riusa la memoria del blocco
 * precedente. currentName, SparseMap e SuccNode allocati nell'arena;
 * nodes[] è del caller e sopravvive al reset.
 * ========================================================================= */
void build_dag(const MachFunction *f, int start, int end, DAGNode *nodes) {
    /* reset O(num_blocchi_arena) ≈ O(1) nel caso comune — riusa memoria */
    arena_reset(s_arena);

    int n        = end - start;
    int universe = f->nextVreg + PHYS_ALLOCATABLE;
    int cap      = universe + n; /* renamed ID in [universe, cap) */

    int *currentName = arena_alloc(s_arena, (size_t)universe * sizeof(int));
    for (int r = 0; r < universe; r++) currentName[r] = r;
    int nextFresh = universe;

    SparseMap smap;
    smap_init(&smap, cap);

    /* Inizializza nodi e marca coppie CMP/TEST+Jcc per macro-fusion */
    for (int i = 0; i < n; i++) {
        nodes[i] = (DAGNode){
            .instrIdx  = start + i,
            .latency   = sched_latency_of(f->instrs[start + i].op),
            .height    = sched_latency_of(f->instrs[start + i].op),
        };
    }
    for (int i = 0; i + 1 < n; i++) {
        if (sched_is_cmp_or_test(f->instrs[start + i].op) &&
            sched_is_jcc(f->instrs[start + i + 1].op))
            nodes[i].pinnedForFusion = 1;
    }

    int lastSideEffect = -1;

    for (int j = 0; j < n; j++) {
        const MachInstr *inj = &f->instrs[start + j];

        /* RAW: risolvi uso nel renamed-space e cerca il definiente */
        int uses[5]; int nuses;
        sched_uses(inj, f->nextVreg, uses, &nuses);
        for (int u = 0; u < nuses; u++) {
            int r   = uses[u];
            int ren = (r >= 0 && r < universe) ? currentName[r] : r;
            int dep = smap_get(&smap, ren);
            if (dep >= 0) dag_add_edge(nodes, dep, j);
        }

        /* Serializzazione effetti collaterali */
        if (sched_has_side_effect(inj->op)) {
            if (lastSideEffect >= 0)
                dag_add_edge(nodes, lastSideEffect, j);
            lastSideEffect = j;
        }

        /* Def: rinomina → elimina WAR/WAW, registra per future RAW.
         * WAW: nuova def dipende dalla vecchia def dello stesso registro. */
        int d = sched_def(inj, f->nextVreg);
        if (d >= 0 && d < universe) {
            int oldRenamed = currentName[d];
            int prevDef    = smap_get(&smap, oldRenamed);
            if (prevDef >= 0) dag_add_edge(nodes, prevDef, j);

            int fresh      = nextFresh++;
            currentName[d] = fresh;
            smap_set(&smap, fresh, j);
        }
    }

    /* Altezze: backward pass (cammino critico ponderato per latenza) */
    for (int i = n - 1; i >= 0; i--) {
        int maxH = 0;
        for (SuccNode *s = nodes[i].succs; s; s = s->next)
            if (nodes[s->to].height > maxH) maxH = nodes[s->to].height;
        nodes[i].height = nodes[i].latency + maxH;
    }
}