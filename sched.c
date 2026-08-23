/**
 * @file sched.c
 * @brief Local list instruction scheduler (per-block, forward, height-ordered).
 *
 * Pipeline: isel_select() -> sched_schedule() -> regalloc(). See sched.h.
 * One Arena per function, reset (not destroyed) between blocks to reuse
 * scratch memory (DAGNode[], result[], heap[], DAG internals).
 */

#include <stdlib.h>
#include <string.h>
#include "sched.h"
#include "sched_dag.h"
#include "sched_utils.h"
#include "arena.h"

/** Instruction range [start, end) for one basic block (mirrors IRBlock layout). */
typedef struct {
    int start;
    int end;
} BasicBlockOffset;

/* partitions f->instrs[] on MACH_LABEL boundaries; caller frees returned array */
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

/* Max-heap on int array, ordered by DAGNode.height (highest first = priority). */

/* push v, sift up O(log n) */
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

/* pop max, sift down O(log n) */
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

/*
 * Forward list scheduling on block [start, end): reorders f->instrs in place.
 * 3 phases: pin structural headers -> greedy heap by height -> flush pinned
 * (control flow, CMP/TEST+Jcc pairs) in original order.
 */
static void schedule_block(MachFunction *f, int start, int end, Arena *arena) {
    int n = end - start;
    if (n <= 1) return;  /* nothing to reorder */

    /* reset (not destroy): reuses same backing memory as previous block */
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

    /* phase 1: pin LABEL/FUNC_BEGIN first, keep out of ready heap */
    for (int i = 0; i < n; i++) {
        MachOp op = f->instrs[start + i].op;
        if (op == MACH_LABEL || op == MACH_FUNC_BEGIN) {
            result[rCount++]   = f->instrs[start + i];
            nodes[i].scheduled = 1;
        }
    }

    /* seed heap: unscheduled, no unresolved preds, not pinned/fused */
    for (int i = 0; i < n; i++) {
        if (nodes[i].scheduled)                        continue;
        if (sched_is_pinned(f->instrs[start + i].op)) continue;
        if (nodes[i].pinnedForFusion)                  continue;
        if (nodes[i].predCount == 0)
            heap_push(heap, &heap_size, i, nodes);
    }

    /* phase 2: pop highest-height ready node, commit, unlock successors */
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

    /* phase 3: flush leftovers (terminators, CMP/TEST+Jcc pairs) in order */
    for (int i = 0; i < n; i++) {
        if (!nodes[i].scheduled)
            result[rCount++] = f->instrs[start + i];
    }

    /* overwrite the original instruction window with the scheduled sequence */
    memcpy(&f->instrs[start], result, (size_t)n * sizeof(MachInstr));
}

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