#include <stdlib.h>
#include <string.h>
#include "sched.h"
#include "sched_dag.h"
#include "sched_utils.h"
#include "arena.h"

typedef struct { int start, end; } BasicBlockOffset;

static BasicBlockOffset *find_basic_blocks(const MachFunction *f, int *outCount) {
    int cap = 8, count = 0;
    BasicBlockOffset *blocks = malloc((size_t)cap * sizeof(BasicBlockOffset));
    int start = 0;

    for (int i = 0; i < f->count; i++) {
        if (f->instrs[i].op == MACH_LABEL && i > start) {
            if (count == cap) {
                cap *= 2;
                blocks = realloc(blocks, (size_t)cap * sizeof(BasicBlockOffset));
            }
            blocks[count++] = (BasicBlockOffset){ start, i };
            start = i;
        }
    }
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
 * schedule_block — list scheduling locale su un singolo blocco base.
 *
 * nodes[] e result[] allocati nella propria arena locale (distrutta a fine
 * funzione). I SuccNode puntati da nodes[i].succs vivono nell'arena interna
 * di sched_dag, che viene resettata all'inizio del build_dag successivo —
 * ma schedule_block consuma il DAG completamente prima di ritornare,
 * quindi nessun dangling pointer.
 * ========================================================================= */
static void schedule_block(MachFunction *f, int start, int end) {
    int n = end - start;
    if (n <= 1) return;

    Arena *arena = arena_create(0);

    DAGNode   *nodes  = arena_alloc(arena, (size_t)n * sizeof(DAGNode));
    MachInstr *result = arena_alloc(arena, (size_t)n * sizeof(MachInstr));
    int rCount = 0;

    build_dag(f, start, end, nodes);   /* reset + ricostruzione DAG */

    MaxHeap heap = {
        .data = arena_alloc(arena, (size_t)n * sizeof(int)),
        .size = 0
    };

    /* 1. Pinned head */
    for (int i = 0; i < n; i++) {
        MachOp op = f->instrs[start + i].op;
        if (op == MACH_LABEL || op == MACH_FUNC_BEGIN) {
            result[rCount++] = f->instrs[start + i];
            nodes[i].scheduled = 1;
        }
    }

    /* Seed ready list */
    for (int i = 0; i < n; i++) {
        if (nodes[i].scheduled)                              continue;
        if (sched_is_pinned(f->instrs[start + i].op))       continue;
        if (nodes[i].pinnedForFusion)                        continue;
        if (nodes[i].predCount == 0)
            heap_push(&heap, i, nodes);
    }

    /* 2. Greedy MaxHeap */
    while (heap.size > 0) {
        int chosen = heap_pop(&heap, nodes);
        result[rCount++] = f->instrs[start + chosen];
        nodes[chosen].scheduled = 1;

        for (SuccNode *s = nodes[chosen].succs; s; s = s->next) {
            int succ = s->to;
            if (nodes[succ].scheduled)                           continue;
            if (sched_is_pinned(f->instrs[start + succ].op))    continue;
            if (nodes[succ].pinnedForFusion)                     continue;
            if (--nodes[succ].predCount == 0)
                heap_push(&heap, succ, nodes);
        }
    }

    /* 3. Pinned tail */
    for (int i = 0; i < n; i++) {
        if (!nodes[i].scheduled)
            result[rCount++] = f->instrs[start + i];
    }

    memcpy(&f->instrs[start], result, (size_t)n * sizeof(MachInstr));
    arena_destroy(arena);
}

/* =========================================================================
 * sched_schedule — entry point pubblico.
 *
 * L'arena di sched_dag viene inizializzata una volta prima del loop
 * e distrutta alla fine: blocchi malloc iniziali mai ripetuti.
 * ========================================================================= */
void sched_schedule(MachProgram *mp) {
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

    sched_dag_arena_fini();
}