/**
 * @file sched_dag.c
 * @brief Dependency DAG construction for the instruction scheduler.
 *
 * See sched_dag.h for the module overview. Internal organisation:
 *   RenameTracker         — per-register bookkeeping (rename generation,
 *                            last writer of each generation, last reader).
 *   track_read/track_write — apply one register access to the tracker,
 *                            emitting RAW/WAR/WAW edges as needed.
 *   dag_add_edge           — dedup'd, arena-allocated edge insertion.
 *   build_dag               — four-pass driver (see header).
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
 * =========================================================================
 * currentName[r]: rename generation currently representing architectural
 *                 register r. Virtual registers start at identity and get a
 *                 fresh generation on every write (see track_write). Physical
 *                 registers NEVER change generation — instr_implicit_uses/defs
 *                 look them up by their fixed id (nextVreg + physReg), so
 *                 renaming them would make that lookup fail.
 * lastWriter[id]: local instruction index that last wrote generation 'id',
 *                 or -1. Indexed directly by rename id (plain array, not a
 *                 sparse map): currentName/lastReader already pay an
 *                 O(universe) init per block, so a sparse structure here
 *                 would only save initialising the few extra "fresh vreg
 *                 generation" slots — not worth the added indirection.
 * lastReader[r]:  local instruction index that last read the CURRENT
 *                 generation of r, or -1. Chaining reads through this field
 *                 (instead of tracking every reader) is what lets a single
 *                 field capture the WAR edge for track_write below.
 * ========================================================================= */
typedef struct {
    int  vregCount;     /**< f->nextVreg: boundary between renamed (vreg) and fixed (phys) ids. */
    int  universe;      /**< vregCount + PHYS_ALLOCATABLE: total architectural register ids. */
    int  nextFresh;      /**< Next rename generation to hand out to a vreg definition. */
    int *currentName;
    int *lastWriter;    /**< Size universe + n: room for one fresh generation per instruction. */
    int *lastReader;
} RenameTracker;

/**
 * @brief Initialise a RenameTracker for a block of @p n instructions.
 *
 * All arrays are arena-allocated and start at "nobody has touched this
 * register yet" (currentName = identity, lastReader/lastWriter = -1).
 */
static RenameTracker rename_tracker_create(int vregCount, int physCount, int n, Arena *arena) {
    int universe = vregCount + physCount;
    int cap = universe + n; // one extra rename generation per instruction is the worst case

    int *currentName = arena_alloc(arena, (size_t)universe * sizeof(int));
    int *lastReader  = arena_alloc(arena, (size_t)universe * sizeof(int));
    int *lastWriter  = arena_alloc(arena, (size_t)cap * sizeof(int));

    // Inizializzazione identità (0, 1, 2, ...)
    for (int r = 0; r < universe; r++) {
        currentName[r] = r;
    }

    // memset a 0xFF imposta tutti i byte a 1, che per gli int in complemento a due equivale a -1
    memset(lastReader, 0xFF, (size_t)universe * sizeof(int));
    memset(lastWriter, 0xFF, (size_t)cap * sizeof(int));

    // Ritorno della struct creata tramite Compound Literal
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
 *
 * Guards self-loops and duplicate edges (linear scan of the — normally
 * short — successor list). Prepended for O(1) insertion.
 */
static void dag_add_edge(DAGNode *nodes, int from, int to, Arena *arena) {
    if (from == to) return;

    for (SuccNode *s = nodes[from].succs; s; s = s->next)
        if (s->to == to) return; // already present: skip

    SuccNode *sn      = arena_alloc(arena, sizeof(SuccNode));
    sn->to            = to;
    sn->next          = nodes[from].succs;
    nodes[from].succs = sn;
    nodes[from].nSuccs++;
    nodes[to].predCount++;
}

/**
 * @brief Record instruction @p j reading architectural register @p r.
 *
 * Emits the RAW edge from r's current-generation writer (if any), then
 * chains this read after the previous one so a later write to r can pick
 * up the WAR edge in O(1) via track_write().
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
 *
 * WAR edge from the last reader of the current generation, WAW edge from
 * the previous writer, then a fresh generation is allocated — for virtual
 * registers only; physical registers keep their fixed id (see RenameTracker doc).
 */
static void track_write(RenameTracker *rt, DAGNode *nodes, Arena *arena, int j, int d) {
    if (d < 0 || d >= rt->universe) return;

    if (rt->lastReader[d] >= 0 && rt->lastReader[d] != j)
        dag_add_edge(nodes, rt->lastReader[d], j, arena); // WAR
    rt->lastReader[d] = -1; // this write starts a fresh generation: no readers yet

    int prevWriter = rt->lastWriter[rt->currentName[d]];
    if (prevWriter >= 0) dag_add_edge(nodes, prevWriter, j, arena); // WAW

    int newGen = (d < rt->vregCount) ? (rt->nextFresh++) : d; // vreg: fresh id; phys: fixed id
    rt->currentName[d]     = newGen;
    rt->lastWriter[newGen] = j;
}

/* =========================================================================
 * build_dag passes — one function per phase (see build_dag at the bottom
 * for the orchestration order and sched_dag.h for the semantic overview).
 * ========================================================================= */

/**
 * @brief Pass 1: initialise every DAGNode with its instruction index and
 *        static latency.
 *
 * height starts equal to latency (a node with no successors has itself as
 * its own critical path); Pass 4 later folds in successor heights.
 *
 * @param f     Machine function owning the instructions.
 * @param blk   Instruction range of the block being processed.
 * @param nodes Output array, one entry per instruction in @p blk.
 * @param n     Number of instructions in @p blk (blk.end - blk.start).
 */
static void dag_init_nodes(const MachFunction *f, BlockRange blk,
                            DAGNode *nodes, int n) {
    for (int i = 0; i < n; i++) {
        MachOpCode op  = f->instrs[blk.start + i].op;
        int    lat = sched_latency_of(op);
        nodes[i] = (DAGNode){ .instrIdx = blk.start + i, .latency = lat, .height = lat };
    }
}

/**
 * @brief Pass 2: mark CMP/TEST instructions immediately followed by a Jcc
 *        as pinned for macro-fusion.
 *
 * A pinned node is excluded from the ready heap (see is_schedulable() in
 * sched.c) so the scheduler never separates the pair, letting the Intel
 * decoder fuse them into a single micro-op.
 *
 * @param f     Machine function owning the instructions.
 * @param blk   Instruction range of the block being processed.
 * @param nodes Node array to annotate (pinnedForFusion field).
 * @param n     Number of instructions in @p blk.
 */
static void dag_pin_fusion_pairs(const MachFunction *f, BlockRange blk,
                                  DAGNode *nodes, int n) {
    for (int i = 0; i + 1 < n; i++)
        if (sched_is_cmp_or_test(f->instrs[blk.start + i].op) &&
            sched_is_jcc(f->instrs[blk.start + i + 1].op))
            nodes[i].pinnedForFusion = 1;
}

/**
 * @brief Pass 3 body: apply one instruction's register/side-effect/memory
 *        dependencies to the DAG.
 *
 * Handles, in order: explicit + implicit register reads (RAW / read-chain
 * via track_read), side-effect serialisation (STORE/PUSH/CALL/IDIV/CQO
 * chained in program order), memory ordering (LOAD/STORE chained — no
 * alias analysis), then explicit + implicit register writes (WAR/WAW via
 * track_write).
 *
 * @param f              Machine function owning the instructions.
 * @param blkStart       First instruction index of the block (f->instrs offset).
 * @param j              Local node index within the block (0-based).
 * @param rt             Rename tracker shared across the whole block.
 * @param nodes          Node array being linked with dependency edges.
 * @param arena          Scratch arena for edge allocation.
 * @param lastSideEffect In/out: node index of the last side-effecting
 *                        instruction seen so far in the block, or -1.
 * @param lastMemoryOp   In/out: node index of the last LOAD/STORE seen so
 *                        far in the block, or -1.
 */
static void dag_process_instr_dependencies(const MachFunction *f, int blkStart, int j,
                                    RenameTracker *rt, DAGNode *nodes, Arena *arena,
                                    int *lastSideEffect, int *lastMemoryOp) {
    const MachInstr *in = &f->instrs[blkStart + j];
    int regs[SCHED_MAX_REG_IDS], nregs;

    // reads: explicit operands, then implicit ABI reads (e.g. CALL args)
    sched_uses(in, f->nextVreg, regs, &nregs);
    for (int k = 0; k < nregs; k++) track_read(rt, nodes, arena, j, regs[k]);

    instr_implicit_uses(in, f->nextVreg, regs, &nregs);
    for (int k = 0; k < nregs; k++) track_read(rt, nodes, arena, j, regs[k]);

    // side-effect serialisation: STORE/PUSH/CALL/IDIV/CQO in program order
    if (sched_has_side_effect(in->op)) {
        if (*lastSideEffect >= 0) dag_add_edge(nodes, *lastSideEffect, j, arena);
        *lastSideEffect = j;
    }

    // memory ordering: no alias analysis, so serialise all LOAD/STORE
    if (in->op == MACH_LOAD || in->op == MACH_STORE) {
        if (*lastMemoryOp >= 0) dag_add_edge(nodes, *lastMemoryOp, j, arena);
        *lastMemoryOp = j;
    }

    // writes: explicit dst, then implicit ABI defs (e.g. CALL clobbers)
    int def = sched_def(in, f->nextVreg);
    if (def >= 0) track_write(rt, nodes, arena, j, def);

    instr_implicit_defs(in, f->nextVreg, regs, &nregs);
    for (int k = 0; k < nregs; k++) track_write(rt, nodes, arena, j, regs[k]);
}

/**
 * @brief Pass 3: build all RAW/WAR/WAW + side-effect + memory ordering
 *        edges for the block.
 *
 * Drives dag_process_instr_dependencies() over every instruction in program order,
 * threading the side-effect/memory-op "last seen" state through the loop.
 *
 * @param f     Machine function owning the instructions.
 * @param blk   Instruction range of the block being processed.
 * @param rt    Rename tracker for the block (already initialised).
 * @param nodes Node array being linked with dependency edges.
 * @param arena Scratch arena for edge allocation.
 * @param n     Number of instructions in @p blk.
 */
static void dag_build_dependencies(const MachFunction *f, BlockRange blk, RenameTracker *rt,
                            DAGNode *nodes, Arena *arena, int n) {
    int lastSideEffect = -1;
    int lastMemoryOp   = -1;

    for (int j = 0; j < n; j++)
        dag_process_instr_dependencies(f, blk.start, j, rt, nodes, arena,
                                &lastSideEffect, &lastMemoryOp);
}

/**
 * @brief Pass 4: propagate critical-path height backward through the DAG.
 *
 * Must run after every edge exists (Pass 3 complete): a node's height is
 * its own latency plus the maximum height among its successors, so nodes
 * must be visited in reverse program order to have successor heights
 * already final.
 *
 * @param nodes Fully-linked node array.
 * @param n     Number of nodes.
 */
static void dag_propagate_heights(DAGNode *nodes, int n) {
    for (int i = n - 1; i >= 0; i--) {
        int maxH = 0;
        for (SuccNode *s = nodes[i].succs; s; s = s->next)
            if (nodes[s->to].height > maxH) maxH = nodes[s->to].height;
        nodes[i].height = nodes[i].latency + maxH;
    }
}

/* =========================================================================
 * build_dag — orchestrates the four passes above (see sched_dag.h for the
 * semantic overview of what each pass models).
 * ========================================================================= */
void build_dag(const MachFunction *f, BlockRange blk, DAGNode *nodes,
               Arena *arena) {
    int n = blk.end - blk.start;

    RenameTracker rt = rename_tracker_create(f->nextVreg, PHYS_ALLOCATABLE, n, arena);

    dag_init_nodes(f, blk, nodes, n);
    dag_pin_fusion_pairs(f, blk, nodes, n);
    dag_build_dependencies(f, blk, &rt, nodes, arena, n);
    dag_propagate_heights(nodes, n);
}