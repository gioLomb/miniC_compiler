#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "dce.h"
#include "liveness.h"
#include "arena.h"

/* ---- Purezza ----------------------------------------------------------- */
static inline int isPure(IROp op) {
    switch (op) {
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_NEG: case IR_NOT:
    case IR_LT:  case IR_LE:  case IR_GT:  case IR_GE:  case IR_EQ: case IR_NE:
    case IR_ASSIGN: case IR_LOAD_ARR:
        return 1;
    default: return 0;
    }
}

/* ---- Reachability ------------------------------------------------------ */
static void markReachableBlocks(IRFunction *f, char *reachable) {
    if (f->blockCount == 0) return;
    int *stack = malloc((size_t)f->blockCount * sizeof(int));
    int top = 0;
    stack[top++] = 0;
    reachable[0] = 1;
    while (top > 0) {
        int b = stack[--top];
        for (int k = 0; k < 2; k++) {
            int s = f->blocks[b].succ[k];
            if (s >= 0 && s < f->blockCount && !reachable[s]) {
                reachable[s] = 1;
                stack[top++] = s;
            }
        }
    }
    free(stack);
}

/* ---- DCE --------------------------------------------------------------- */
int dce_optimize(IRFunction *f) {
    if (!f || f->blockCount == 0 || f->count == 0) return 0;

    int nBlocks = f->blockCount;
    int nInstrs = f->count;

    Arena *arena = arena_create(0);

    /* PASSO 0: reachability */
    char *reachable = arena_alloc(arena, (size_t)nBlocks);
    memset(reachable, 0, (size_t)nBlocks);
    markReachableBlocks(f, reachable);

    /* PASSO 1-4: liveness (modulo condiviso) */
    LivenessResult liv = liveness_compute(f, reachable, arena);
    int words   = liv.words;

    /* PASSO 5: Mark — scansione backward dentro ogni blocco */
    char *eliminate = arena_alloc(arena, (size_t)nInstrs);
    memset(eliminate, 0, (size_t)nInstrs);

    for (int b = 0; b < nBlocks; b++) {
        if (!reachable[b]) {
            for (int i = f->blocks[b].start; i < f->blocks[b].end; i++)
                eliminate[i] = 1;
            continue;
        }

        LiveSet live = liveset_new(arena, words);
        liveset_copy(&live, &liv.LiveOut[b]);

        for (int i = f->blocks[b].end - 1; i >= f->blocks[b].start; i--) {
            IRInstr *in = &f->instrs[i];
            int def   = liveness_defines_dst(in->op) && liveness_is_var_or_temp(in->dst.kind);
            int dstId = def ? varmap_operand_id(&liv.varMap, in->dst) : -1;

            if (isPure(in->op) && def && dstId >= 0 && !liveset_test(&live, dstId)) {
                eliminate[i] = 1;
                continue;
            }
            if (liveness_is_var_or_temp(in->src1.kind)) {
                int id = varmap_operand_id(&liv.varMap, in->src1);
                if (id >= 0) liveset_set(&live, id);
            }
            if (liveness_is_var_or_temp(in->src2.kind)) {
                int id = varmap_operand_id(&liv.varMap, in->src2);
                if (id >= 0) liveset_set(&live, id);
            }
            if (def && dstId >= 0) liveset_clrbit(&live, dstId);
        }
    }

    /* PASSO 6: Sweep */
    int *map = arena_alloc(arena, (size_t)nInstrs * sizeof(int));
    for (int i = 0; i < nInstrs; i++) map[i] = -1;

    IRInstr *newInstrs = malloc((size_t)nInstrs * sizeof(IRInstr));
    int newCount = 0;
    for (int i = 0; i < nInstrs; i++) {
        if (!eliminate[i]) { newInstrs[newCount] = f->instrs[i]; map[i] = newCount++; }
    }
    free(f->instrs);
    f->instrs   = newInstrs;
    f->count    = newCount;
    f->capacity = newCount;

    for (int b = 0; b < nBlocks; b++) {
        int oldStart = f->blocks[b].start, oldEnd = f->blocks[b].end;
        int newStart = -1, newEnd = -1;
        for (int i = oldStart; i < oldEnd; i++) {
            if (map[i] != -1) {
                if (newStart == -1) newStart = map[i];
                newEnd = map[i] + 1;
            }
        }
        f->blocks[b].start = (newStart == -1) ? 0 : newStart;
        f->blocks[b].end   = (newEnd   == -1) ? 0 : newEnd;
    }
    f->curBlockStart = 0;

    /* PASSO 7: Pulizia */
    arena_destroy(arena);
    varmap_destroy(&liv.varMap);
    return newCount != nInstrs;
}