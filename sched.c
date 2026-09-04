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

typedef struct {
    int           *data;   /**< Backing array, arena-allocated by the caller (capacity instrCount). */
    int            size;   /**< Number of entries currently in the heap.                            */
    const DAGNode *nodes;  /**< DAGNode array indexed by the values stored in data[].                */
} ReadyHeap;

/**
 * @brief Partition f->instrs[] into basic blocks on MACH_LABEL boundaries.
 *
 * A label always opens a new block (labels are jump targets, so control
 * flow can enter there from elsewhere); every other instruction stays in
 * the block it was found in.
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
    if (start < f->count) {
        outBlocks[count++] = (BlockRange){ start, f->count };
    }

    return count;
}



/**
 * @brief Build an empty ReadyHeap over @p backing (capacity == block size).
 */
static ReadyHeap ready_heap_create(int *backing, const DAGNode *nodes) {
    return (ReadyHeap){ .data = backing, .size = 0, .nodes = nodes };
}

static inline int ready_heap_height(const ReadyHeap *h, int nodeIdx) {
    return h->nodes[nodeIdx].height;
}

static inline void ready_heap_swap(int *data, int i, int j) {
    int tmp = data[i];
    data[i] = data[j];
    data[j] = tmp;
}

/**
 * @brief Restore max-heap invariant upward O(log n).
 */
static void ready_heap_sift_up(ReadyHeap *h, int index) {
    int i = index;
    while (i > 0) {
        int parent = (i - 1) / 2;
        if (ready_heap_height(h, h->data[parent]) >= ready_heap_height(h, h->data[i])) {
            break;
        }
        ready_heap_swap(h->data, parent, i);
        i = parent;
    }
}

/**
 * @brief Restore max-heap invariant downward O(log n).
 */
static void ready_heap_sift_down(ReadyHeap *h, int index) {
    int i = index;
    for (;;) {
        int l = 2 * i + 1;
        int r = 2 * i + 2;
        int best = i;

        if (l < h->size && ready_heap_height(h, h->data[l]) > ready_heap_height(h, h->data[best])) {
            best = l;
        }
        if (r < h->size && ready_heap_height(h, h->data[r]) > ready_heap_height(h, h->data[best])) {
            best = r;
        }
        if (best == i) {
            break; /* Proprietà dell'heap soddisfatta */
        }

        ready_heap_swap(h->data, i, best);
        i = best;
    }
}

/**
 * @brief Push element v into the heap.
 */
static void ready_heap_push(ReadyHeap *h, int v) {
    int i = h->size++;
    h->data[i] = v;
    ready_heap_sift_up(h, i);
}

/**
 * @brief Pop maximum element from the heap.
 */
static int ready_heap_pop(ReadyHeap *h) {
    int top = h->data[0];
    h->data[0] = h->data[--h->size];
    ready_heap_sift_down(h, 0);
    return top;
}


/**
 * @brief Returns 1 if node i is a candidate for the ready heap based on its
 *        static/scheduling-state properties alone (NOT predCount).
 */
static inline int is_schedulable(const MachInstr *instr, const DAGNode *node) {
    return !node->scheduled && !node->pinnedForFusion && !sched_is_pinned(instr->op);
}

static inline int is_pinned_header(MachOpCode op) {
    return op == MACH_LABEL || op == MACH_FUNC_BEGIN;
}

/**
 * @brief Place structural headers (LABEL, FUNC_BEGIN) first, unconditionally.
 */
static int emit_pinned_headers(const MachInstr *src, int instrCount,
                                DAGNode *nodes, MachInstr *result) {
    int rCount = 0;
    for (int i = 0; i < instrCount; i++) {
        const MachInstr in = src[i];
        if (is_pinned_header(in.op)) {
            result[rCount++]   = in;
            nodes[i].scheduled = 1;
        }
    }
    return rCount;
}

/**
 * @brief Push every node that can legally start the greedy loop.
 */
static void seed_ready_heap(const MachInstr *src, int instrCount,
                             DAGNode *nodes, ReadyHeap *heap) {
    for (int i = 0; i < instrCount; i++) {
        if (is_schedulable(&src[i], &nodes[i]) && nodes[i].predCount == 0) {
            ready_heap_push(heap, i);
        }
    }
}

/**
 * @brief Unlocks successor nodes when predecessor nodeIdx is scheduled.
 */
static void unlock_successors(int nodeIdx, const MachInstr *src, DAGNode *nodes, ReadyHeap *heap) {
    for (SuccNode *s = nodes[nodeIdx].succs; s; s = s->next) {
        int succ = s->to;
        nodes[succ].predCount--;
        if (nodes[succ].predCount == 0 && is_schedulable(&src[succ], &nodes[succ])) {
            ready_heap_push(heap, succ);
        }
    }
}

/**
 * @brief Greedily commit ready nodes, highest critical-path height first.
 */
static int run_list_scheduling(const MachInstr *src, DAGNode *nodes,
                                ReadyHeap *heap, MachInstr *result, int rCount) {
    while (heap->size > 0) {
        int chosen = ready_heap_pop(heap);
        result[rCount++]        = src[chosen];
        nodes[chosen].scheduled = 1;

        unlock_successors(chosen, src, nodes, heap);
    }
    return rCount;
}

/**
 * @brief Append every instruction the greedy loop never touched.
 */
static int flush_leftovers(const MachInstr *src, int instrCount,
                            const DAGNode *nodes, MachInstr *result, int rCount) {
    for (int i = 0; i < instrCount; i++) {
        if (!nodes[i].scheduled) {
            result[rCount++] = src[i];
        }
    }
    return rCount;
}

/**
 * @brief Forward list scheduling on block [blk.start, blk.end).
 */
static void schedule_block(MachFunction *f, BlockRange blk, Arena *arena) {
    int instrCount = blk.end - blk.start;
    if (instrCount <= 1) return; // Nothing to reorder

    arena_reset(arena); // Reuse the same backing memory as the previous block

    const MachInstr *src = &f->instrs[blk.start];
    DAGNode   *nodes  = arena_alloc(arena, (size_t)instrCount * sizeof(DAGNode));
    MachInstr *result = arena_alloc(arena, (size_t)instrCount * sizeof(MachInstr));

    build_dag(f, blk, nodes, arena);

    ReadyHeap heap = ready_heap_create(arena_alloc(arena, (size_t)instrCount * sizeof(int)), nodes);

    int rCount = emit_pinned_headers(src, instrCount, nodes, result);
    seed_ready_heap(src, instrCount, nodes, &heap);
    rCount = run_list_scheduling(src, nodes, &heap, result, rCount);
    rCount = flush_leftovers(src, instrCount, nodes, result, rCount);

    memcpy(&f->instrs[blk.start], result, (size_t)instrCount * sizeof(MachInstr));
}

/* =========================================================================
 * Public entry point
 * ========================================================================= */

void sched_schedule(MachProgram *mp) {
    if (!mp || mp->count == 0) return;

    // Hoisting delle risorse: le Arena vengono create una sola volta
    // e azzerate ad ogni iterazione, azzerando l'overhead di allocazione OS.
    Arena *blockArena   = arena_create(0);
    Arena *scratchArena = arena_create(0);

    const int funcCount = mp->count;
    MachFunction **functions = mp->functions;

    for (int fi = 0; fi < funcCount; fi++) {
        MachFunction *f = functions[fi];
        
        // Inversione della guardia: ramo caldo nel fall-through path
        if (f && f->count > 0) {
            arena_reset(blockArena);
            
            const size_t allocSize = (size_t)f->count * sizeof(BlockRange);
            BlockRange *blocks = arena_alloc(blockArena, allocSize);
            
            const int bbCount = find_basic_blocks(f, blocks);

            for (int b = 0; b < bbCount; b++) {
                schedule_block(f, blocks[b], scratchArena);
            }
        }
    }

    arena_destroy(scratchArena);
    arena_destroy(blockArena);
}