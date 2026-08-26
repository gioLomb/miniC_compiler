/**
 * @file sched.c
 * @brief Local list instruction scheduler (per-block, forward, height-ordered).
 *
 * Pipeline: isel_select() -> sched_schedule() -> regalloc(). See sched.h.
 *
 * Algorithm per basic block (schedule_block):
 *   1. build_dag()           — dependency DAG with RAW/WAR/WAW + ordering edges
 *                               (sched_dag.h/.c). BlockRange passed as a whole
 *                               struct so the [start,end) pair never drifts
 *                               out of sync between caller and callee.
 *   2. emit_pinned_headers() — LABEL/FUNC_BEGIN go first, unconditionally,
 *                               marked scheduled so they never enter the heap.
 *   3. seed_ready_heap()     — every remaining unpinned node with zero
 *                               unresolved predecessors becomes an initial
 *                               candidate.
 *   4. run_list_scheduling() — pop the highest-height ready node, commit it,
 *                               unlock successors whose last predecessor
 *                               just committed.
 *   5. flush_leftovers()     — whatever never entered the heap (terminators,
 *                               CMP/TEST pinned for macro-fusion) is appended
 *                               in original program order.
 *
 * One Arena per function, reset (not destroyed) between blocks to reuse
 * scratch memory (DAGNode[], result[], heap backing array, DAG internals).
 */

#include <string.h>
#include "sched.h"
#include "sched_dag.h"
#include "sched_utils.h"
#include "arena.h"

/* =========================================================================
 * Basic-block partitioning
 * ========================================================================= */

/**
 * @brief Partition f->instrs[] into basic blocks on MACH_LABEL boundaries.
 *
 * A label always opens a new block (labels are jump targets, so control
 * flow can enter there from elsewhere); every other instruction stays in
 * the block it was found in.
 *
 * Writes each block's [start,end) range into @p outBlocks, which the
 * caller must size for at least f->count entries: a block can never span
 * zero instructions, so there can never be more blocks than instructions —
 * no growth/realloc bookkeeping is needed here.
 *
 * @return Number of blocks written into @p outBlocks.
 */
static int find_basic_blocks(const MachFunction *f, BlockRange *outBlocks) {
    int count = 0;
    int start = 0;

    for (int i = 0; i < f->count; i++) {
        if (f->instrs[i].op == MACH_LABEL && i > start) {
            outBlocks[count++] = (BlockRange){ start, i };
            start = i;
        }
    }
    if (start < f->count)
        outBlocks[count++] = (BlockRange){ start, f->count };

    return count;
}

/* =========================================================================
 * ReadyHeap — max-heap over DAGNode indices, ordered by height
 * =========================================================================
 * Standard binary heap stored in a flat array. Bundled with the DAGNode
 * array it compares against so call sites never juggle (data, size, nodes)
 * as three separate parameters.
 * ========================================================================= */

typedef struct {
    int           *data;   /**< Backing array, arena-allocated by the caller (capacity instrCount). */
    int            size;   /**< Number of entries currently in the heap.                            */
    const DAGNode *nodes;  /**< DAGNode array indexed by the values stored in data[].                */
} ReadyHeap;

/**
 * @brief Build an empty ReadyHeap over @p backing (capacity == block size).
 *
 * Returned by value (like buckets_create()): the heap's state (size, the
 * comparison array) is small and fully known at construction time, so
 * there is nothing an in-place init would save over just handing back
 * the finished struct.
 */
static ReadyHeap ready_heap_create(int *backing, const DAGNode *nodes) {
    return (ReadyHeap){ .data = backing, .size = 0, .nodes = nodes };
}

static inline int ready_heap_height(const ReadyHeap *h, int nodeIdx) {
    return h->nodes[nodeIdx].height;
}

/* push v, sift up O(log n): swap with parent while parent has lower height */
static void ready_heap_push(ReadyHeap *h, int v) {
    int i = h->size++;
    h->data[i] = v;

    while (i > 0) {
        int parent = (i - 1) / 2;
        if (ready_heap_height(h, h->data[parent]) >= ready_heap_height(h, h->data[i])) break;
        int t = h->data[parent]; h->data[parent] = h->data[i]; h->data[i] = t;
        i = parent;
    }
}

/* pop max, sift down O(log n): swap with the larger child until heap property holds */
static int ready_heap_pop(ReadyHeap *h) {
    int top = h->data[0];
    h->data[0] = h->data[--h->size];

    int i = 0;
    for (;;) {
        int l = 2 * i + 1, r = 2 * i + 2, best = i;
        if (l < h->size && ready_heap_height(h, h->data[l]) > ready_heap_height(h, h->data[best])) best = l;
        if (r < h->size && ready_heap_height(h, h->data[r]) > ready_heap_height(h, h->data[best])) best = r;
        if (best == i) break; /* heap property satisfied */
        int t = h->data[best]; h->data[best] = h->data[i]; h->data[i] = t;
        i = best;
    }
    return top;
}

/* =========================================================================
 * Per-block scheduling phases
 * ========================================================================= */

 /*
 * Returns 1 if node i is a candidate for the ready heap based on its
 * static/scheduling-state properties alone (NOT predCount — the seed loop
 * and the successor-unlock loop check predCount differently: seed checks
 * it as-is, successors check it right after a decrement — so that part
 * stays explicit at each call site instead of being hidden in here).
 */
static inline int is_schedulable(const MachInstr src, const DAGNode node) {
    return !(node.scheduled || sched_is_pinned(src.op) || node.pinnedForFusion);
}


/**
 * @brief Place structural headers (LABEL, FUNC_BEGIN) first, unconditionally.
 *
 * They must open the block; marking them scheduled here keeps them out of
 * the ready heap entirely (seed_ready_heap skips anything already scheduled).
 *
 * @return Number of entries written into @p result (starts at 0: this is
 *         always the first phase to run).
 */
static int emit_pinned_headers(const MachInstr *src, int instrCount,
                                DAGNode *nodes, MachInstr *result) {
    int rCount = 0;
    for (int i = 0; i < instrCount; i++) {
        MachOp op = src[i].op;
        if (op != MACH_LABEL && op != MACH_FUNC_BEGIN) continue;
        result[rCount++]   = src[i];
        nodes[i].scheduled = 1;
    }
    return rCount;
}

/**
 * @brief Push every node that can legally start the greedy loop.
 *
 * A node is an initial candidate iff: not already scheduled (headers),
 * not pinned by sched_is_pinned (control-flow terminators — must stay
 * last, handled by flush_leftovers), not pinned for CMP/TEST+Jcc fusion,
 * and has no unresolved predecessor left.
 */
static void seed_ready_heap(const MachInstr *src, int instrCount,
                             DAGNode *nodes, ReadyHeap *heap) {
    for (int i = 0; i < instrCount; i++) {
        if(is_schedulable(src[i],nodes[i]) && nodes[i].predCount == 0) ready_heap_push(heap, i);   
    }
}

/**
 * @brief Greedily commit ready nodes, highest critical-path height first.
 *
 * Committing a node can make its successors ready: each successor's
 * predCount is decremented, and once it reaches zero (this was its last
 * unresolved predecessor) the successor joins the heap — unless it is
 * itself pinned, in which case flush_leftovers() places it later.
 *
 * @param rCount  Number of entries already written into @p result.
 * @return        Updated count after this phase's entries.
 */
static int run_list_scheduling(const MachInstr *src, DAGNode *nodes,
                                ReadyHeap *heap, MachInstr *result, int rCount) {
    while (heap->size > 0) {
        int chosen = ready_heap_pop(heap);
        result[rCount++]        = src[chosen];
        nodes[chosen].scheduled = 1;

        for (SuccNode *s = nodes[chosen].succs; s; s = s->next) {
            int succ = s->to;
            if (--nodes[succ].predCount == 0 && is_schedulable(src[succ],nodes[succ])) ready_heap_push(heap, succ);
        }
    }
    return rCount;
}

/**
 * @brief Append every instruction the greedy loop never touched.
 *
 * Covers control-flow terminators (sched_is_pinned) and CMP/TEST pinned
 * for macro-fusion — both must keep their original relative order, which
 * a plain forward scan preserves automatically.
 *
 * @param rCount  Number of entries already written into @p result.
 * @return        Updated count after this phase's entries.
 */
static int flush_leftovers(const MachInstr *src, int instrCount,
                            const DAGNode *nodes, MachInstr *result, int rCount) {
    for (int i = 0; i < instrCount; i++)
        if (!nodes[i].scheduled) result[rCount++] = src[i];
    return rCount;
}

/*
 * Forward list scheduling on block [blk.start, blk.end): reorders f->instrs
 * in place via the four phases above.
 */
static void schedule_block(MachFunction *f, BlockRange blk, Arena *arena) {
    int instrCount = blk.end - blk.start;
    if (instrCount <= 1) return; // nothing to reorder

    arena_reset(arena); // reuse the same backing memory as the previous block

    const MachInstr *src = &f->instrs[blk.start];
    DAGNode   *nodes  = arena_alloc(arena, (size_t)instrCount * sizeof(DAGNode));
    MachInstr *result = arena_alloc(arena, (size_t)instrCount * sizeof(MachInstr));

    // dependency DAG: all RAW/WAR/WAW + ordering edges, all arena-allocated;
    // blk passed whole so [start,end) can never drift between caller/callee
    build_dag(f, blk, nodes, arena);

    ReadyHeap heap = ready_heap_create(arena_alloc(arena, (size_t)instrCount * sizeof(int)), nodes);

    int rCount = emit_pinned_headers(src, instrCount, nodes, result);
    seed_ready_heap(src, instrCount, nodes, &heap);
    rCount = run_list_scheduling(src, nodes, &heap, result, rCount);
    rCount = flush_leftovers(src, instrCount, nodes, result, rCount);

    // overwrite the original instruction window with the scheduled sequence
    memcpy(&f->instrs[blk.start], result, (size_t)instrCount * sizeof(MachInstr));
}

/* =========================================================================
 * Public entry point
 * ========================================================================= */

void sched_schedule(MachProgram *mp) {
    for (int fi = 0; fi < mp->count; fi++) {
        MachFunction *f = mp->functions[fi];
        if (!f || f->count == 0) continue;

        Arena *blockArena = arena_create(0);
        BlockRange *blocks = arena_alloc(blockArena, (size_t)f->count * sizeof(BlockRange));
        int bbCount = find_basic_blocks(f, blocks);

        Arena *scratchArena = arena_create(0);
        for (int b = 0; b < bbCount; b++)
            schedule_block(f, blocks[b], scratchArena);

        arena_destroy(scratchArena);
        arena_destroy(blockArena);
    }
}