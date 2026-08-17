/**
 * @file sched.c
 * @brief Local list instruction scheduler — implementation.
 *
 * See sched.h for the module overview and public API documentation.
 *
 * Internal organisation
 * ---------------------
 *  1. BasicBlockOffset / find_basic_blocks — block-boundary identification.
 *  2. heap_push / heap_pop                — max-heap priority queue helpers.
 *  3. schedule_block                      — per-block list scheduling driver.
 *  4. sched_schedule                      — public entry point (iterates
 *                                           functions and blocks).
 *
 * Algorithm overview
 * ------------------
 * For each basic block, the scheduler performs forward list scheduling using
 * a max-heap ordered by latency-weighted critical-path height (DAGNode.height).
 * The algorithm proceeds in three phases:
 *
 *  Phase 1 — Pin structural headers (MACH_LABEL, MACH_FUNC_BEGIN):
 *    These instructions must always be first; they are placed into the result
 *    array immediately and marked scheduled before the heap is seeded.
 *
 *  Phase 2 — Greedy list scheduling:
 *    All instructions whose predecessors have been committed (predCount == 0)
 *    and that are neither globally pinned (control flow, returns) nor pinned
 *    for macro-fusion (CMP/TEST awaiting its Jcc) are eligible for the ready
 *    heap.  Each step pops the highest-height ready node, commits it, and
 *    decrements the predCount of its successors, potentially making more
 *    nodes ready.
 *
 *  Phase 3 — Flush remaining pinned instructions:
 *    Globally pinned nodes (Jcc, JMP, RET) and macro-fusion pairs that were
 *    excluded from the heap are emitted in their original program order.
 *    This guarantees that CMP/TEST always immediately precedes its Jcc and
 *    that all control-flow terminators appear at the end of the block.
 *
 * Memory management
 * -----------------
 * One Arena is created per machine function and reset (not destroyed) between
 * basic blocks.  This amortises allocation overhead: the same backing memory
 * is reused for scratch arrays (DAGNode[], result[], heap[]) and DAG internals
 * (SparseMap, SuccNode objects, currentName table) across all blocks of the
 * same function.  The arena is destroyed once the function is fully scheduled.
 *
 * Pipeline position
 * -----------------
 *   isel_select() → sched_schedule() → regalloc()
 */

#include <stdlib.h>
#include <string.h>
#include "sched.h"
#include "sched_dag.h"
#include "sched_utils.h"
#include "arena.h"

/* =========================================================================
 * Basic-block boundary identification
 * =========================================================================
 * The scheduler operates on individual basic blocks.  A basic block
 * boundary is detected when a MACH_LABEL instruction is encountered at any
 * position after the block's start: the label begins a new block, so the
 * current block closes at that index.  The trailing instructions after the
 * last label (or the whole function if no labels are present) form the final
 * block.
 * ========================================================================= */

/**
 * @brief Internal descriptor for a basic-block instruction range.
 *
 * The block covers f->instrs[start .. end-1] (start inclusive, end exclusive).
 * This mirrors the layout used by the IR-level IRBlock to allow uniform
 * treatment across both representations.
 */
typedef struct {
    int start; /**< Index of the first instruction in the block (inclusive). */
    int end;   /**< Index one past the last instruction in the block.        */
} BasicBlockOffset;

/**
 * @brief Partition a machine function's instruction stream into basic blocks.
 *
 * Scans f->instrs[] for MACH_LABEL instructions; each label (other than one
 * at position 0) closes the current block and opens a new one.  The last
 * open block is closed at f->count.
 *
 * The returned array is heap-allocated and must be freed by the caller
 * (sched_schedule() does this with a plain free()).
 *
 * @param f        Machine function to partition.
 * @param outCount Set to the number of BasicBlockOffset entries returned.
 * @return         Heap-allocated array of BasicBlockOffset; caller must free.
 */
static BasicBlockOffset *find_basic_blocks(const MachFunction *f, int *outCount) {
    int cap   = 8;
    int count = 0;
    BasicBlockOffset *blocks = malloc((size_t)cap * sizeof(BasicBlockOffset));
    int start = 0;

    for (int i = 0; i < f->count; i++) {
        if (f->instrs[i].op == MACH_LABEL && i > start) {
            /* close current block before the label */
            if (count == cap) {
                cap   *= 2;
                blocks = realloc(blocks, (size_t)cap * sizeof(BasicBlockOffset));
            }
            blocks[count++] = (BasicBlockOffset){ start, i };
            start = i;  /* new block begins at the label */
        }
    }

    /* close the last (or only) block */
    if (start < f->count) {
        if (count == cap) {
            cap   *= 2;
            blocks = realloc(blocks, (size_t)cap * sizeof(BasicBlockOffset));
        }
        blocks[count++] = (BasicBlockOffset){ start, f->count };
    }

    *outCount = count;
    return blocks;
}

/* =========================================================================
 * Max-heap priority queue
 * =========================================================================
 * A binary max-heap stored in a plain int array, ordered by DAGNode.height.
 * This is intentionally a simple pair of file-static functions rather than
 * a struct: the heap is always used together with the local nodes[] array
 * and its size is tracked by the caller, so no encapsulation overhead is
 * needed.
 *
 * Priority key: DAGNode.height (latency-weighted critical-path length).
 * Scheduling the highest-height node first minimises the expected pipeline
 * stall cycles by starting the longest dependency chains as early as
 * possible within the instruction window.
 * ========================================================================= */

/**
 * @brief Push DAG node index @p v onto the max-heap and restore the heap property.
 *
 * Appends @p v at the current tail of the array, then sifts it upward by
 * swapping with its parent until the heap property (parent.height >=
 * child.height) is restored.  O(log n) worst case.
 *
 * @param data   Int array backing the heap.
 * @param size   Pointer to the current heap size; incremented on return.
 * @param v      DAGNode index to insert.
 * @param nodes  DAGNode array; provides the height values used for ordering.
 */
static void heap_push(int *data, int *size, int v, const DAGNode *nodes) {
    int i = (*size)++;
    data[i] = v;

    /* sift up: swap with parent while parent has lower height */
    while (i > 0) {
        int p = (i - 1) >> 1;  /* parent index */
        if (nodes[data[p]].height >= nodes[data[i]].height) break;
        int t = data[p]; data[p] = data[i]; data[i] = t;
        i = p;
    }
}

/**
 * @brief Remove and return the DAG node index with the greatest height.
 *
 * Swaps the root with the last element, decrements the size, then sifts the
 * new root down by repeatedly swapping with the larger child until the heap
 * property is restored.  O(log n) worst case.
 *
 * @param data   Int array backing the heap.
 * @param size   Pointer to the current heap size; decremented on return.
 * @param nodes  DAGNode array; provides the height values used for ordering.
 * @return       Index of the DAGNode with the maximum height.
 */
static int heap_pop(int *data, int *size, const DAGNode *nodes) {
    int top = data[0];
    data[0] = data[--(*size)];

    /* sift down: swap with the larger child while heap property is violated */
    for (int i = 0;;) {
        int l = 2 * i + 1;   /* left child  */
        int r = 2 * i + 2;   /* right child */
        int b = i;            /* index of the largest among i, l, r */

        if (l < *size && nodes[data[l]].height > nodes[data[b]].height) b = l;
        if (r < *size && nodes[data[r]].height > nodes[data[b]].height) b = r;
        if (b == i) break;    /* heap property satisfied */

        int t = data[b]; data[b] = data[i]; data[i] = t;
        i = b;
    }
    return top;
}

/* =========================================================================
 * Per-block list scheduling
 * =========================================================================
 * schedule_block() orchestrates the three-phase algorithm for a single
 * basic block.  All scratch memory (DAGNode[], result[], heap[]) as well
 * as all DAG-internal allocations (SparseMap, SuccNode list) live in the
 * shared Arena, which is reset at the start of each block invocation so
 * that memory from the previous block is reclaimed without a destroy/create
 * pair.
 * ========================================================================= */

/**
 * @brief Apply forward list scheduling to the basic block [start, end).
 *
 * Rewrites f->instrs[start .. end-1] in place with the scheduled order.
 * Blocks of size <= 1 are trivially schedulable and returned immediately.
 *
 * Memory usage: all scratch arrays are arena-allocated; build_dag() also
 * uses the same arena for its internal SparseMap and SuccNode objects.
 * arena_reset() at the top of this function reclaims all memory from the
 * previous block before any new allocation takes place.
 *
 * Scheduling phases:
 *
 *  Phase 1 — Structural headers (MACH_LABEL, MACH_FUNC_BEGIN):
 *    Must be first; placed into result[] immediately and flagged scheduled.
 *    Not added to the heap; they have fixed positions by definition.
 *
 *  Phase 2 — Greedy list scheduling (ready heap):
 *    After the DAG is built, seed the heap with all unpinned nodes that have
 *    predCount == 0 (no unsatisfied dependencies).  Each iteration pops the
 *    highest-height ready node, commits it, and checks each successor: if
 *    decrementing its predCount brings it to zero and it is not pinned, it
 *    becomes ready and is pushed onto the heap.
 *
 *  Phase 3 — Remaining pinned instructions (original order):
 *    Any node not yet scheduled after phase 2 (globally pinned control-flow
 *    terminators and macro-fusion pairs) is appended in its original index
 *    order.  This preserves the CMP/TEST + Jcc adjacency required for Intel
 *    macro-fusion and ensures terminators (JMP, Jcc, RET) remain last.
 *
 * @param f      Machine function being scheduled (modified in place).
 * @param start  First instruction index of the block (inclusive).
 * @param end    One-past-last instruction index of the block (exclusive).
 * @param arena  Shared arena; reset at entry to reclaim previous block memory.
 */
static void schedule_block(MachFunction *f, int start, int end, Arena *arena) {
    int n = end - start;
    if (n <= 1) return;  /* trivially scheduled; nothing to reorder */

    /*
     * Reclaim all memory from the previous block.  This does not free the
     * arena's backing pages — it only resets the write pointer, so the next
     * allocations reuse the same memory without any syscall overhead.
     */
    arena_reset(arena);

    /* scratch arrays for this block — all arena-allocated */
    DAGNode   *nodes  = arena_alloc(arena, (size_t)n * sizeof(DAGNode));
    MachInstr *result = arena_alloc(arena, (size_t)n * sizeof(MachInstr));
    int        rCount = 0;

    /* ready-heap backing array; worst case all n nodes are simultaneously ready */
    int *heap      = arena_alloc(arena, (size_t)n * sizeof(int));
    int  heap_size = 0;

    /* build the dependency DAG; all DAG-internal allocations go into arena */
    build_dag(f, start, end, nodes, arena);

    /* ------------------------------------------------------------------
     * Phase 1: pin structural block-entry instructions.
     *
     * MACH_LABEL and MACH_FUNC_BEGIN must be first in the output.  Placing
     * them before the heap is seeded ensures the heap only contains
     * schedulable (non-structural) instructions.
     * ------------------------------------------------------------------ */
    for (int i = 0; i < n; i++) {
        MachOp op = f->instrs[start + i].op;
        if (op == MACH_LABEL || op == MACH_FUNC_BEGIN) {
            result[rCount++]   = f->instrs[start + i];
            nodes[i].scheduled = 1;
        }
    }

    /*
     * Seed the ready heap with all nodes that have:
     *   - No unscheduled predecessors (predCount == 0)
     *   - Not globally pinned (control-flow terminators must stay last)
     *   - Not pinned for macro-fusion (must stay adjacent to its Jcc)
     */
    for (int i = 0; i < n; i++) {
        if (nodes[i].scheduled)                        continue;
        if (sched_is_pinned(f->instrs[start + i].op)) continue;
        if (nodes[i].pinnedForFusion)                  continue;
        if (nodes[i].predCount == 0)
            heap_push(heap, &heap_size, i, nodes);
    }

    /* ------------------------------------------------------------------
     * Phase 2: greedy list scheduling driven by critical-path height.
     *
     * Each iteration commits the highest-priority ready node and exposes
     * new ready nodes by decrementing their predecessors' counts.
     * ------------------------------------------------------------------ */
    while (heap_size > 0) {
        int chosen = heap_pop(heap, &heap_size, nodes);
        result[rCount++]        = f->instrs[start + chosen];
        nodes[chosen].scheduled = 1;

        /* expose successors that have no remaining unscheduled predecessors */
        for (SuccNode *s = nodes[chosen].succs; s; s = s->next) {
            int succ = s->to;
            if (nodes[succ].scheduled)                        continue;
            if (sched_is_pinned(f->instrs[start + succ].op)) continue;
            if (nodes[succ].pinnedForFusion)                  continue;
            if (--nodes[succ].predCount == 0)
                heap_push(heap, &heap_size, succ, nodes);
        }
    }

    /* ------------------------------------------------------------------
     * Phase 3: flush remaining pinned and fused-pair instructions.
     *
     * Any node not yet committed is either a globally pinned terminator
     * (JMP, Jcc, RET) or a CMP/TEST paired with a Jcc.  Appending them in
     * their original index order preserves program correctness:
     *   - Terminators always appear at the block's tail.
     *   - The CMP/TEST immediately precedes its Jcc, enabling macro-fusion.
     * ------------------------------------------------------------------ */
    for (int i = 0; i < n; i++) {
        if (!nodes[i].scheduled)
            result[rCount++] = f->instrs[start + i];
    }

    /* overwrite the original instruction window with the scheduled sequence */
    memcpy(&f->instrs[start], result, (size_t)n * sizeof(MachInstr));
}

/* =========================================================================
 * Public entry point
 * ========================================================================= */

/**
 * @brief Schedule all basic blocks of every function in @p mp.
 *
 * Iterates over each MachFunction in the program.  For each function, one
 * Arena is created and shared across all blocks (reset between them to
 * avoid accumulation).  The basic-block boundaries are identified by
 * find_basic_blocks() and the per-block scheduler is invoked for each.
 *
 * The arena is destroyed once all blocks of the function have been scheduled,
 * releasing all scratch memory in a single operation.  The heap-allocated
 * BasicBlockOffset array is freed with a plain free() since it is not arena-
 * managed.
 *
 * @param mp  Machine program to schedule (modified in place).
 */
void sched_schedule(MachProgram *mp) {
    for (int fi = 0; fi < mp->count; fi++) {
        MachFunction *f = mp->functions[fi];
        if (!f || f->count == 0) continue;

        /* One arena per function: reset between blocks, destroyed after. */
        Arena *arena = arena_create(0);

        int bbCount = 0;
        BasicBlockOffset *blocks = find_basic_blocks(f, &bbCount);

        for (int b = 0; b < bbCount; b++)
            schedule_block(f, blocks[b].start, blocks[b].end, arena);

        free(blocks);
        arena_destroy(arena);
    }
}