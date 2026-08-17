#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sched.h"
#include "sched_dag.h"
#include "sched_utils.h"
#include "arena.h"

/**
 * @brief Internal descriptor representing a basic block boundary within an instruction stream.
 */
typedef struct {
    int start; /**< Inclusive start instruction index. */
    int end;   /**< Exclusive end instruction index.   */
} BasicBlockOffset;

/**
 * @brief Scans a machine function's instruction sequence and partitions it into basic blocks.
 *
 * Basic block boundaries are delimited by `MACH_LABEL` instructions or control flow boundaries.
 *
 * @param f        Pointer to the machine function to partition.
 * @param outCount Output parameter populated with the total number of identified basic blocks.
 * @return Dynamically allocated array of `BasicBlockOffset` structures (caller must free).
 */
static BasicBlockOffset *find_basic_blocks(const MachFunction *f, int *outCount) {
    int cap = 8, count = 0;
    BasicBlockOffset *blocks = malloc((size_t)cap * sizeof(BasicBlockOffset));
    int start = 0;

    for (int i = 0; i < f->count; i++) {
        // A label starts a new basic block boundary if preceded by instructions
        if (f->instrs[i].op == MACH_LABEL && i > start) {
            if (count == cap) {
                cap *= 2;
                blocks = realloc(blocks, (size_t)cap * sizeof(BasicBlockOffset));
            }
            blocks[count++] = (BasicBlockOffset){ start, i };
            start = i;
        }
    }
    // Capture trailing basic block sequence
    if (start < f->count) {
        if (count == cap) {
            cap *= 2;
            blocks = realloc(blocks, (size_t)cap * sizeof(BasicBlockOffset));
        }
        blocks[count++] = (BasicBlockOffset){ start, f->count };
    }
    *outCount = count;
    return blocks;
}

/* =========================================================================
 * Local Basic Block List Scheduling
 * ========================================================================= */

/**
 * @brief Performs forward list scheduling on a single basic block range [start, end).
 *
 * Allocates workspace `nodes` and `result` buffers within a scratch arena. Edge lists
 * point into the global `sched_dag` arena, which is reset on subsequent builds.
 * Because scheduling consumes the DAG completely before returning, no dangling pointers persist.
 *
 * @param f     Pointer to the machine function being modified.
 * @param start Inclusive start index of the basic block.
 * @param end   Exclusive end index of the basic block.
 */
static void schedule_block(MachFunction *f, int start, int end) {
    int n = end - start;
    if (n <= 1) return; // Single-instruction or empty blocks require no reordering

    Arena *arena = arena_create(0);

    DAGNode   *nodes  = arena_alloc(arena, (size_t)n * sizeof(DAGNode));
    MachInstr *result = arena_alloc(arena, (size_t)n * sizeof(MachInstr));
    int rCount = 0;

    // Build the dependency DAG (resets module arena and populates nodes array)
    build_dag(f, start, end, nodes);

    MaxHeap heap = {
        .data = arena_alloc(arena, (size_t)n * sizeof(int)),
        .size = 0
    };

    // 1. Schedule pinned block entry headers (e.g., function prologue or labels)
    for (int i = 0; i < n; i++) {
        MachOp op = f->instrs[start + i].op;
        if (op == MACH_LABEL || op == MACH_FUNC_BEGIN) {
            result[rCount++] = f->instrs[start + i];
            nodes[i].scheduled = 1;
        }
    }

    // Seed ready heap with root instructions (zero unscheduled predecessors)
    for (int i = 0; i < n; i++) {
        if (nodes[i].scheduled)                          continue;
        if (sched_is_pinned(f->instrs[start + i].op))   continue;
        if (nodes[i].pinnedForFusion)                    continue;
        if (nodes[i].predCount == 0)
            heap_push(&heap, i, nodes);
    }

    // 2. Greedy list scheduling driven by critical path priority (MaxHeap)
    while (heap.size > 0) {
        int chosen = heap_pop(&heap, nodes);
        result[rCount++] = f->instrs[start + chosen];
        nodes[chosen].scheduled = 1;

        // Decrement predecessor degree for all dependent successor nodes
        for (SuccNode *s = nodes[chosen].succs; s; s = s->next) {
            int succ = s->to;
            if (nodes[succ].scheduled)                       continue;
            if (sched_is_pinned(f->instrs[start + succ].op))continue;
            if (nodes[succ].pinnedForFusion)                 continue;
            if (--nodes[succ].predCount == 0)
                heap_push(&heap, succ, nodes);
        }
    }

    // 3. Schedule pinned block tail and remaining instructions (e.g., jumps, fused pairs)
    for (int i = 0; i < n; i++) {
        if (!nodes[i].scheduled)
            result[rCount++] = f->instrs[start + i];
    }

    // Write reordered instruction stream back to the target function stream
    memcpy(&f->instrs[start], result, (size_t)n * sizeof(MachInstr));
    arena_destroy(arena);
}

/* =========================================================================
 * Public Interface
 * ========================================================================= */

void sched_schedule(MachProgram *mp) {
    // Initialize module-level DAG arena once across all basic blocks
    sched_dag_arena_init();

    for (int fi = 0; fi < mp->count; fi++) {
        MachFunction *f = mp->functions[fi];
        if (!f || f->count == 0) continue;

        int bbCount = 0;
        BasicBlockOffset *blocks = find_basic_blocks(f, &bbCount);
        for (int b = 0; b < bbCount; b++)
            schedule_block(f, blocks[b].start, blocks[b].end);
        free(blocks);
    }

    // Release DAG module internal arena
    sched_dag_arena_fini();
}