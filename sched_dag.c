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
 *              RAW edges via currentName[] + SparseMap lookup per source reg,
 *              for both EXPLICIT operand uses (sched_uses) and IMPLICIT ABI
 *              uses (instr_implicit_uses — e.g. CALL reading argument regs).
 *              Side-effect serialisation via lastSideEffect chain.
 *              Memory ordering serialisation via lastMemoryOp chain.
 *              WAW edge from previous writer + fresh rename id per definition,
 *              for both the EXPLICIT dst and any IMPLICIT def
 *              (instr_implicit_defs — e.g. CALL writing %rax on return).
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
 * Implicit ABI uses/defs (CALL, IDIV, CQO)
 * -----------------------------------------
 * MACH_CALL does not carry the argument registers (%rdi, %rsi, ...) or the
 * return register (%rax) as explicit src1/src2/dst operands — the calling
 * convention places them there implicitly.  sched_uses()/sched_def() only
 * see explicit operands, so without extra handling the scheduler has no edge
 * between the MOVs that load the argument registers and the CALL, nor
 * between the CALL and the MOV that reads %rax afterwards.  This lets the
 * greedy list scheduler hoist CALL before its arguments are ready (or the
 * result-copy before the call has even run), corrupting the calling
 * convention while still producing a structurally valid schedule.
 *
 * Fixed here by additionally consulting instr_implicit_uses()/
 * instr_implicit_defs() (regalloc_utils.h) — the same functions the register
 * allocator already uses to model CALL/IDIV/CQO's implicit register
 * traffic for liveness/interference — and feeding their results through the
 * identical RAW/WAW + renaming machinery used for explicit operands.
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
#include "regalloc_utils.h"  /* instr_implicit_uses/defs: usi/def impliciti (es. CALL
                               * legge i registri argomento e scrive %rax) non visibili
                               * in src1/src2/dst, quindi invisibili a sched_uses()/
                               * sched_def(). Riusati qui invece di duplicare la logica
                               * (gia' corretta) usata dal register allocator. */

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
 * Per-register read/write dependency helpers
 * =========================================================================
 * Small helpers factoring out the RAW+WAR bookkeeping for one read, and
 * the WAR-drain+WAW+rename bookkeeping for one write, so build_dag()'s
 * main loop stays readable when applied to both explicit and implicit
 * operand sets.
 * ========================================================================= */

/**
 * @brief Record instruction @p j reading architectural register @p r.
 *
 * Emits the RAW edge from r's current writer (if any), then chains this
 * read after the previous one via lastReader[r] (WAR bookkeeping: see
 * module header for why chaining reads is enough to cover all of them).
 */
static void dag_process_read(DAGNode *nodes, Arena *arena, int universe,
                              const int *currentName, int *lastReader,
                              const SparseMap *smap, int j, int r) {
    if (r < 0 || r >= universe) return;

    int dep = smap_get(smap, currentName[r]);
    if (dep >= 0) dag_add_edge(nodes, dep, j, arena); // RAW

    if (lastReader[r] >= 0) dag_add_edge(nodes, lastReader[r], j, arena); // read-after-read chain
    lastReader[r] = j;
}

/**
 * @brief Record instruction @p j writing architectural register @p d.
 *
 * Drains lastReader[d] into a WAR edge (this def must follow every earlier
 * read, transitively via the chain built by dag_process_read), emits the
 * WAW edge from the previous writer, then allocates a fresh rename id.
 */
static void dag_process_write(DAGNode *nodes, Arena *arena, int universe,
                               int *currentName, int *lastReader,
                               SparseMap *smap, int *nextFresh, int j, int d) {
    if (d < 0 || d >= universe) return;

    if (lastReader[d] >= 0 && lastReader[d] != j) {
        dag_add_edge(nodes, lastReader[d], j, arena); // WAR
    }
    lastReader[d] = -1; // fresh generation

    int prevDef = smap_get(smap, currentName[d]);
    if (prevDef >= 0) dag_add_edge(nodes, prevDef, j, arena); // WAW

    // FIX RENAMING:
    // I registri virtuali (< nextVreg) usano il renaming dinamico (fresh).
    // I registri FISICI (>= nextVreg, es. RAX) NON devono cambiare ID (niente fresh),
    // altrimenti le implicit_uses/defs ABI non li troveranno piu'!
    int renameId;
    if (d < (universe - PHYS_ALLOCATABLE)) {
        renameId = (*nextFresh)++;
    } else {
        renameId = d; // Mantiene l'id fisso per i registri fisici
    }

    currentName[d] = renameId;
    smap_set(smap, renameId, j);
}

/* =========================================================================
 * DAG construction — four passes
 * ========================================================================= */

void build_dag(const MachFunction *f, int start, int end, DAGNode *nodes,
               Arena *arena) {
    int n        = end - start;
    int universe = f->nextVreg + PHYS_ALLOCATABLE;
    int cap      = universe + n;  // n extra slots for fresh rename ids

    // currentName[r]: current rename generation of architectural register r.
    int *currentName = arena_alloc(arena, (size_t)universe * sizeof(int));
    for (int r = 0; r < universe; r++) currentName[r] = r;
    int nextFresh = universe;

    // lastReader[r]: most recent node that read r since its last definition
    // (-1 = none). See module header for the WAR-via-chaining rationale.
    int *lastReader = arena_alloc(arena, (size_t)universe * sizeof(int));
    for (int r = 0; r < universe; r++) lastReader[r] = -1;

    SparseMap smap;
    smap_init(&smap, cap, arena);

    /* ---- Pass 1: node initialisation ---- */
    for (int i = 0; i < n; i++) {
        nodes[i] = (DAGNode){
            .instrIdx = start + i,
            .latency  = sched_latency_of(f->instrs[start + i].op),
            .height   = sched_latency_of(f->instrs[start + i].op),
        };
    }

    /* ---- Pass 2: macro-fusion scan ---- */
    for (int i = 0; i + 1 < n; i++) {
        if (sched_is_cmp_or_test(f->instrs[start + i].op) &&
            sched_is_jcc(f->instrs[start + i + 1].op))
            nodes[i].pinnedForFusion = 1;
    }

    /* ---- Pass 3: RAW + WAR + WAW + side-effect/memory ordering ---- */
    int lastSideEffect = -1;
    int lastMemoryOp   = -1;

    for (int j = 0; j < n; j++) {
        const MachInstr *inj = &f->instrs[start + j];

        // reads: explicit operands, then implicit ABI reads (e.g. CALL args)
        int uses[5], nuses;
        sched_uses(inj, f->nextVreg, uses, &nuses);
        for (int u = 0; u < nuses; u++)
            dag_process_read(nodes, arena, universe, currentName, lastReader,
                              &smap, j, uses[u]);

        int iuses[16], niuses;
        instr_implicit_uses(inj, f->nextVreg, iuses, &niuses);
        for (int u = 0; u < niuses; u++)
            dag_process_read(nodes, arena, universe, currentName, lastReader,
                              &smap, j, iuses[u]);

        // side-effect serialisation (STORE/PUSH/CALL/IDIV/CQO in program order)
        if (sched_has_side_effect(inj->op)) {
            if (lastSideEffect >= 0) dag_add_edge(nodes, lastSideEffect, j, arena);
            lastSideEffect = j;
        }

        // memory ordering (no alias analysis: serialise all LOAD/STORE)
        if (inj->op == MACH_LOAD || inj->op == MACH_STORE) {
            if (lastMemoryOp >= 0) dag_add_edge(nodes, lastMemoryOp, j, arena);
            lastMemoryOp = j;
        }

        // writes: explicit dst, then implicit ABI defs (e.g. CALL clobbers)
        int explicitDef = sched_def(inj, f->nextVreg);
        if (explicitDef >= 0)
            dag_process_write(nodes, arena, universe, currentName, lastReader,
                               &smap, &nextFresh, j, explicitDef);

        int idefs[16], nidefs;
        instr_implicit_defs(inj, f->nextVreg, idefs, &nidefs);
        for (int k = 0; k < nidefs; k++){
            dag_process_write(nodes, arena, universe, currentName, lastReader,
                               &smap, &nextFresh, j, idefs[k]);}
            printf("INSTR %d (%d): def=%d | uses=", j, inj->op, explicitDef);
for (int u = 0; u < nuses; u++){ printf("%d ", uses[u]);}
printf("\n");
    }

    /* ---- Pass 4: backward height propagation ---- */
    for (int i = n - 1; i >= 0; i--) {
        int maxH = 0;
        for (SuccNode *s = nodes[i].succs; s; s = s->next)
            if (nodes[s->to].height > maxH) maxH = nodes[s->to].height;
        nodes[i].height = nodes[i].latency + maxH;
    }
}