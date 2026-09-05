/**
 * @file sched_dag.c
 * @brief Dependency DAG construction for the instruction scheduler.
 *
 * See sched_dag.h for the module overview. Internal organisation:
 *   RenameTracker                 — per-register bookkeeping (rename generation,
 *                                    last writer of each generation, last reader).
 *   track_read/track_write         — apply one register access to the tracker,
 *                                    emitting RAW/WAR/WAW edges as needed.
 *   dag_add_edge                   — dedup'd, arena-allocated edge insertion.
 *   build_dag                     — four-pass driver (see header).
 */

#include <string.h>
#include "sched_dag.h"
#include "sched_utils.h"
#include "regalloc_utils.h"   /* instr_implicit_uses/defs: ABI-implicit register
                                * traffic (CALL args/return, IDIV/CQO RAX:RDX)
                                * invisible to sched_uses()/sched_def(). Reused
                                * from the register allocator instead of
                                * duplicating the logic. */

/* =========================================================================
 * RenameTracker
 * ========================================================================= */
typedef struct {
    int  vregCount;     /**< f->nextVreg: boundary between renamed (vreg) and fixed (phys) ids. */
    int  universe;      /**< vregCount + PHYS_ALLOCATABLE: total architectural register ids. */
    int  nextFresh;     /**< Next rename generation to hand out to a vreg definition. */
    int *currentName;
    int *lastWriter;    /**< Size universe + n: room for one fresh generation per instruction. */
    int *lastReader;
} RenameTracker;

/**
 * @brief Initialise a RenameTracker for a block of @p n instructions.
 */
static RenameTracker rename_tracker_create(int vregCount, int physCount, int n, Arena *arena) {
    int universe = vregCount + physCount;
    int cap = universe + n; // worst case: one extra rename generation per instruction

    int *currentName = arena_alloc(arena, (size_t)universe * sizeof(int));
    int *lastReader  = arena_alloc(arena, (size_t)universe * sizeof(int));
    int *lastWriter  = arena_alloc(arena, (size_t)cap * sizeof(int));

    // Inizializzazione identità (0, 1, 2, ...)
    for (int r = 0; r < universe; r++) {
        currentName[r] = r;
    }

    // memset a -1 imposta tutti i byte a 0xFF (in complemento a due equivale a -1)
    memset(lastReader, -1, (size_t)universe * sizeof(int));
    memset(lastWriter, -1, (size_t)cap * sizeof(int));

    return (RenameTracker){
        .vregCount   = vregCount,
        .universe    = universe,
        .nextFresh   = universe,
        .currentName = currentName,
        .lastReader  = lastReader,
        .lastWriter  = lastWriter
    };
}

/**
 * @brief Add a directed dependency edge from node @p from to node @p to.
 */
static void dag_add_edge(DAGNode *nodes, int from, int to, Arena *arena) {
    if (from == to) return;

    // O(1) dedup via bitset instead of walking the successor list
    if (bitset_test(&nodes[from].connectedTo, to)) return;
    bitset_set(&nodes[from].connectedTo, to);

    SuccNode *sn      = arena_alloc(arena, sizeof(SuccNode));
    sn->to            = to;
    sn->next          = nodes[from].succs;
    nodes[from].succs = sn;
    nodes[from].nSuccs++;
    nodes[to].predCount++;
}

/**
 * @brief Record instruction @p j reading architectural register @p r.
 */
static void track_read(RenameTracker *rt, DAGNode *nodes, Arena *arena, int j, int r) {
    if (r < 0 || r >= rt->universe) return;

    int writer = rt->lastWriter[rt->currentName[r]];
    if (writer >= 0) dag_add_edge(nodes, writer, j, arena); // RAW

    if (rt->lastReader[r] >= 0) dag_add_edge(nodes, rt->lastReader[r], j, arena); // read chain
    rt->lastReader[r] = j;
}

/**
 * @brief Record instruction @p j writing architectural register @p d.
 */
static void track_write(RenameTracker *rt, DAGNode *nodes, Arena *arena, int j, int d) {
    if (d < 0 || d >= rt->universe) return;

    if (rt->lastReader[d] >= 0 && rt->lastReader[d] != j) {
        dag_add_edge(nodes, rt->lastReader[d], j, arena); // WAR
    }
    rt->lastReader[d] = -1; // this write starts a fresh generation: no readers yet

    int prevWriter = rt->lastWriter[rt->currentName[d]];
    if (prevWriter >= 0) dag_add_edge(nodes, prevWriter, j, arena); // WAW

    int newGen = (d < rt->vregCount) ? (rt->nextFresh++) : d; // vreg: fresh id; phys: fixed id
    rt->currentName[d]     = newGen;
    rt->lastWriter[newGen] = j;
}

/* =========================================================================
 * Helper orientati all'espressività e all'eliminazione delle duplicazioni
 * ========================================================================= */

static inline void track_reg_list_reads(RenameTracker *rt, DAGNode *nodes, Arena *arena,
                                         int j, const int *regs, int count) {
    for (int k = 0; k < count; k++) {
        track_read(rt, nodes, arena, j, regs[k]);
    }
}

static inline void track_reg_list_writes(RenameTracker *rt, DAGNode *nodes, Arena *arena,
                                          int j, const int *regs, int count) {
    for (int k = 0; k < count; k++) {
        track_write(rt, nodes, arena, j, regs[k]);
    }
}



static inline int sched_is_memory_op(MachOpCode op) {
    return op == MACH_LOAD || op == MACH_STORE;
}

static inline int get_max_successor_height(const DAGNode *node, const DAGNode *nodes) {
    int maxH = 0;
    for (const SuccNode *s = node->succs; s; s = s->next) {
        if (nodes[s->to].height > maxH) {
            maxH = nodes[s->to].height;
        }
    }
    return maxH;
}


/**
 * @brief initialise every DAGNode with its instruction index, static latency,
 *        and an empty "already connected to" dedup bitset.
 *
 * All n bitsets are carved out of one contiguous arena slab (single alloc)
 * instead of n separate arena_alloc calls — same slab pattern used by
 * liveness.c for per-block bit-word arrays.
 */
static void dag_init_nodes(const MachFunction *f, BlockRange blk,
                            DAGNode *nodes, int n, Arena *arena) {
    int words = (n + 63) / 64;
    uint64_t *slab = arena_alloc(arena, (size_t)n * (size_t)words * sizeof(uint64_t));
    memset(slab, 0, (size_t)n * (size_t)words * sizeof(uint64_t));

    for (int i = 0; i < n; i++) {
        MachOpCode op  = f->instrs[blk.start + i].op;
        int        lat = sched_latency_of(op);
        nodes[i] = (DAGNode){ .instrIdx = blk.start + i, .latency = lat, .height = lat };
        // point this node's dedup bitset into its slice of the shared slab
        nodes[i].connectedTo = (BitSet){ slab + (size_t)i * words, words };
    }
}


/**
 * @brief  mark CMP/TEST instructions immediately followed by a Jcc as pinned for macro-fusion.
 */
static void dag_pin_fusion_pairs(const MachFunction *f, BlockRange blk,
                                  DAGNode *nodes, int n) {
    for (int i = 0; i + 1 < n; i++) {
        MachOpCode curr = f->instrs[blk.start + i].op;
        MachOpCode next = f->instrs[blk.start + i + 1].op;
        if (sched_is_cmp_or_test(curr) && sched_is_jcc(next)) {
            nodes[i].pinnedForFusion = 1;
        }
    }
}

/**
 * @brief apply one instruction's register/side-effect/memory dependencies to the DAG.
 */
static void dag_process_instr_dependencies(const MachFunction *f, BlockRange blk, int j,
                                            RenameTracker *rt, DAGNode *nodes, Arena *arena,
                                            int *lastSideEffect, int *lastMemoryOp) {
    const MachInstr *in = &f->instrs[blk.start + j];
    int regs[SCHED_MAX_REG_IDS], nregs;

    // 1. explicit operands, then implicit ABI reads (e.g. CALL args)
    sched_uses(in, f->nextVreg, regs, &nregs);
    track_reg_list_reads(rt, nodes, arena, j, regs, nregs);

    instr_implicit_uses(in, f->nextVreg, regs, &nregs);
    track_reg_list_reads(rt, nodes, arena, j, regs, nregs);

    // Side-effect serialisation: STORE/PUSH/CALL/IDIV/CQO in program order
    if (sched_has_side_effect(in->op)) {
        if (*lastSideEffect >= 0) dag_add_edge(nodes, *lastSideEffect, j, arena);
        *lastSideEffect = j;
    }

    // Memory ordering: no alias analysis, so serialise all LOAD/STORE
    if (sched_is_memory_op(in->op)) {
        if (*lastMemoryOp >= 0) dag_add_edge(nodes, *lastMemoryOp, j, arena);
        *lastMemoryOp = j;
    }

    // Writes: explicit dst, then implicit ABI defs
    int def = sched_def(in, f->nextVreg);
    if (def >= 0) track_write(rt, nodes, arena, j, def);

    instr_implicit_defs(in, f->nextVreg, regs, &nregs);
    track_reg_list_writes(rt, nodes, arena, j, regs, nregs);
}

/**
 * @brief build all RAW/WAR/WAW + side-effect + memory ordering edges for the block.
 */
static void dag_build_dependencies(const MachFunction *f, BlockRange blk, RenameTracker *rt,
                                    DAGNode *nodes, Arena *arena, int n) {
    int lastSideEffect = -1;
    int lastMemoryOp   = -1;

    for (int j = 0; j < n; j++) {
        dag_process_instr_dependencies(f, blk, j, rt, nodes, arena,
                                        &lastSideEffect, &lastMemoryOp);
    }
}

/**
 * @brief propagate critical-path height backward through the DAG.
 */
static void dag_propagate_heights(DAGNode *nodes, int n) {
    for (int i = n - 1; i >= 0; i--) {
        nodes[i].height = nodes[i].latency + get_max_successor_height(&nodes[i], nodes);
    }
}


void build_dag(const MachFunction *f, BlockRange blk, DAGNode *nodes,
               Arena *arena) {
    int n = blk.end - blk.start;

    RenameTracker rt = rename_tracker_create(f->nextVreg, PHYS_ALLOCATABLE, n, arena);

    dag_init_nodes(f, blk, nodes, n, arena);
    dag_pin_fusion_pairs(f, blk, nodes, n);
    dag_build_dependencies(f, blk, &rt, nodes, arena, n);
    dag_propagate_heights(nodes, n);
}