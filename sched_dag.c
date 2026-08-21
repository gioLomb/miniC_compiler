/**
 * @file sched_dag.c
 * @brief Dependency DAG construction for the instruction scheduler — implementation.
 *
 * See sched_dag.h for the module overview and full API documentation.
 *
 * Internal organisation
 * ---------------------
 *  smap_init / smap_get / smap_set  — O(1) sparse-set-backed integer map.
 *  dag_add_edge                      — guarded edge insertion (dedup + arena alloc).
 *  build_dag                         — four-pass DAG construction driver.
 *
 * Four-pass algorithm inside build_dag()
 * ---------------------------------------
 *  Pass 1 — Node initialisation: instrIdx, latency, initial height.
 *  Pass 2 — Macro-fusion scan: pin CMP/TEST nodes that precede a Jcc.
 *  Pass 3 — Forward edge-building with register renaming:
 *              RAW edges via currentName[] + SparseMap lookup per source reg.
 *              Side-effect serialisation via lastSideEffect chain.
 *              Memory ordering serialisation via lastMemoryOp chain.
 *              WAW edge from previous writer + fresh rename id per definition.
 *  Pass 4 — Backward height propagation: height = latency + max(succ heights).
 *
 * Register renaming (pass 3 detail)
 * ----------------------------------
 * The universe has (nextVreg + PHYS_ALLOCATABLE) architectural ids, plus up
 * to n fresh ids (one per definition in the block, worst case).  The SparseMap
 * is therefore initialised with capacity = universe + n.
 *
 * currentName[r] maps architectural register r to its current rename id.
 * Initialised to currentName[r] = r (identity).  On a definition of r:
 *   1. Look up oldRenamed = currentName[r] in the SparseMap; if it has a
 *      previous writer, emit a WAW edge from that writer to the current node.
 *   2. Allocate fresh = nextFresh++ as the new rename id for r.
 *   3. Update currentName[r] = fresh and record this node as the writer.
 * On a use of r: resolve currentName[r] and emit a RAW edge from its writer.
 *
 * This scheme eliminates WAR edges between virtual registers and reduces WAW
 * edges to those that are truly unavoidable.
 *
 * Memory ordering (pass 3 detail)
 * --------------------------------
 * Without alias analysis we cannot determine whether two memory operations
 * access overlapping locations.  A conservative lastMemoryOp chain ensures
 * every LOAD and STORE is serialised relative to all preceding LOAD/STOREs,
 * preventing the scheduler from reordering them in ways that could violate
 * load/store semantics.
 */

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
    // sparse[] intentionally left uninitialised: stale values are harmless
    // because membership is validated by the round-trip dense[sparse[k]] == k
}

int smap_get(const SparseMap *m, int k) {
    // cast to unsigned: rejects negative k with a single comparison
    if ((unsigned)k >= (unsigned)m->cap) return -1;
    unsigned pos = (unsigned)m->sparse[k];
    // round-trip validation: stale sparse[k] entries point outside [0,n) or
    // to a slot whose dense[] entry holds a different key
    if (pos >= (unsigned)m->n || m->dense[pos] != k) return -1;
    return m->val[pos];
}

void smap_set(SparseMap *m, int k, int v) {
    if ((unsigned)k >= (unsigned)m->cap) return;
    unsigned pos = (unsigned)m->sparse[k];
    if (pos < (unsigned)m->n && m->dense[pos] == k) {
        // key already present: update value in-place (no layout change)
        m->val[pos] = v;
        return;
    }
    // new key: append to the dense/val arrays and record its position
    m->sparse[k]   = m->n;
    m->dense[m->n] = k;
    m->val[m->n]   = v;
    m->n++;
}

/* =========================================================================
 * DAG edge insertion
 * ========================================================================= */

/**
 * @brief Add a directed dependency edge from node @p from to node @p to.
 *
 * Guards against self-loops (from == to) and duplicate edges: the successor
 * list is walked to check for an existing edge before allocating a SuccNode.
 * Edges are prepended to the singly-linked list for O(1) insertion.
 *
 * @param nodes  DAGNode array for the current block.
 * @param from   Source node index (predecessor).
 * @param to     Destination node index (dependent successor).
 * @param arena  Arena from which the new SuccNode is allocated.
 */
static void dag_add_edge(DAGNode *nodes, int from, int to, Arena *arena) {
    if (from == to) return;

    // dedup: skip if edge already exists (successor list is short in practice)
    for (SuccNode *s = nodes[from].succs; s; s = s->next)
        if (s->to == to) return;

    SuccNode *sn      = arena_alloc(arena, sizeof(SuccNode));
    sn->to            = to;
    sn->next          = nodes[from].succs;  // prepend for O(1) insertion
    nodes[from].succs = sn;
    nodes[from].nSuccs++;
    nodes[to].predCount++;
}

/* =========================================================================
 * DAG construction — four passes
 * ========================================================================= */

void build_dag(const MachFunction *f, int start, int end, DAGNode *nodes,
               Arena *arena) {
    int n         = end - start;
    int universe  = f->nextVreg + PHYS_ALLOCATABLE;
    int cap       = universe + n;  // n extra slots for fresh rename ids

    // currentName[r]: maps architectural register r to its current rename id.
    // Initialised to identity (currentName[r] = r) — no renaming yet.
    int *currentName = arena_alloc(arena, (size_t)universe * sizeof(int));
    for (int r = 0; r < universe; r++) currentName[r] = r;
    int nextFresh = universe;  // next available rename id beyond the universe

    // SparseMap: rename id → index of last instruction that defined it
    SparseMap smap;
    smap_init(&smap, cap, arena);

    /* ------------------------------------------------------------------
     * Pass 1 — Node initialisation
     * Set instrIdx, latency, and initial height (= latency for sink nodes);
     * compound-literal assignment zeroes all pointer / counter fields.
     * ------------------------------------------------------------------ */
    for (int i = 0; i < n; i++) {
        nodes[i] = (DAGNode){
            .instrIdx = start + i,
            .latency  = sched_latency_of(f->instrs[start + i].op),
            .height   = sched_latency_of(f->instrs[start + i].op),
        };
    }

    /* ------------------------------------------------------------------
     * Pass 2 — Macro-fusion scan
     * Mark CMP/TEST nodes immediately followed by a Jcc with pinnedForFusion=1
     * so the scheduler never separates them (Intel macro-fusion requires
     * adjacency for the decoder to merge the pair into one micro-op).
     * ------------------------------------------------------------------ */
    for (int i = 0; i + 1 < n; i++) {
        if (sched_is_cmp_or_test(f->instrs[start + i].op) &&
            sched_is_jcc(f->instrs[start + i + 1].op))
            nodes[i].pinnedForFusion = 1;
    }

    /* ------------------------------------------------------------------
     * Pass 3 — Forward edge-building with register renaming
     * For each instruction j (in program order):
     *   a) Emit RAW edges from the last writer of each source register.
     *   b) Chain after lastSideEffect if j has side effects.
     *   c) Chain after lastMemoryOp if j is a memory operation.
     *   d) Emit a WAW edge from the previous writer of the defined register,
     *      then allocate a fresh rename id and update currentName / SparseMap.
     * ------------------------------------------------------------------ */
    int lastSideEffect = -1;  // index of the most recent side-effecting node
    int lastMemoryOp   = -1;  // index of the most recent LOAD or STORE node

    for (int j = 0; j < n; j++) {
        const MachInstr *inj = &f->instrs[start + j];

        /* a) RAW edges: resolve each source to its renamed id and look up
         *    the last writer in the SparseMap. */
        int uses[5], nuses;
        sched_uses(inj, f->nextVreg, uses, &nuses);
        for (int u = 0; u < nuses; u++) {
            int r   = uses[u];
            int ren = (r >= 0 && r < universe) ? currentName[r] : r;
            int dep = smap_get(&smap, ren);
            if (dep >= 0) dag_add_edge(nodes, dep, j, arena);
        }

        /* b) Side-effect serialisation: chain all side-effecting instructions
         *    in program order to prevent illegal reordering of memory writes,
         *    calls, and divide/sign-extend sequences. */
        if (sched_has_side_effect(inj->op)) {
            if (lastSideEffect >= 0)
                dag_add_edge(nodes, lastSideEffect, j, arena);
            lastSideEffect = j;
        }

        /* c) Memory ordering: serialise every LOAD and STORE relative to all
         *    preceding memory operations.  Without alias analysis we cannot
         *    prove that two accesses are to disjoint locations, so we take
         *    the conservative approach. */
        if (inj->op == MACH_LOAD || inj->op == MACH_STORE) {
            if (lastMemoryOp >= 0)
                dag_add_edge(nodes, lastMemoryOp, j, arena);
            lastMemoryOp = j;
        }

        /* d) WAW edge + rename: if the same architectural register was written
         *    earlier in this block, emit a WAW edge from that writer.  Then
         *    retire the old rename id and publish a fresh one so future uses
         *    of this register pick up the new writer via RAW edges. */
        int d = sched_def(inj, f->nextVreg);
        if (d >= 0 && d < universe) {
            int oldRenamed = currentName[d];
            int prevDef    = smap_get(&smap, oldRenamed);
            // WAW: this definition must follow the previous one for the same reg
            if (prevDef >= 0) dag_add_edge(nodes, prevDef, j, arena);

            // allocate a fresh rename id to eliminate future WAR edges
            int fresh      = nextFresh++;
            currentName[d] = fresh;
            smap_set(&smap, fresh, j);
        }
    }

    /* ------------------------------------------------------------------
     * Pass 4 — Backward height propagation
     * height[i] = latency[i] + max(height[s] for s in succs[i])
     * Scanning in reverse order ensures all successors are already finalised
     * when their predecessor is processed.
     * ------------------------------------------------------------------ */
    for (int i = n - 1; i >= 0; i--) {
        int maxH = 0;
        for (SuccNode *s = nodes[i].succs; s; s = s->next)
            if (nodes[s->to].height > maxH) maxH = nodes[s->to].height;
        // add own latency on top of the longest outgoing path
        nodes[i].height = nodes[i].latency + maxH;
    }
}