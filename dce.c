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
            int s = f->blocks[b].bb.succ[k];
            if (s >= 0 && s < f->blockCount && !reachable[s]) {
                reachable[s] = 1;
                stack[top++] = s;
            }
        }
    }
    free(stack);
}

/* ---- PASSO 5: Mark (backward scan per ogni blocco) --------------------- */
static void dce_mark(IRFunction *f, const char *reachable, LivenessResult *liv,
                      Arena *arena, char *eliminate) {
    int nBlocks = f->blockCount;
    int words   = liv->blockSets.words;
    int nInstrs = f->count;

    memset(eliminate, 0, (size_t)nInstrs);

    for (int b = 0; b < nBlocks; b++) {
        if (!reachable[b]) {
            for (int i = f->blocks[b].bb.start; i < f->blocks[b].bb.end; i++)
                eliminate[i] = 1;
            continue;
        }

        LiveSet live = liveset_new(arena, words);
        liveset_copy(&live, &liv->blockSets.LiveOut[b]);

        for (int i = f->blocks[b].bb.end - 1; i >= f->blocks[b].bb.start; i--) {
            IRInstr *in = &f->instrs[i];
            int def   = liveness_defines_dst(in->op) && liveness_is_var_or_temp(in->dst.kind);
            int dstId = def ? varmap_operand_id(&liv->varMap, in->dst) : -1;

            /* Istituzione pura con dst morta → eliminabile */
            if (isPure(in->op) && def && dstId >= 0 && !liveset_test(&live, dstId)) {
                eliminate[i] = 1;
                continue;
            }

            /* Aggiorna Live: src → vivi, dst → morto */
            if (liveness_is_var_or_temp(in->src1.kind)) {
                int id = varmap_operand_id(&liv->varMap, in->src1);
                if (id >= 0) liveset_set(&live, id);
            }
            if (liveness_is_var_or_temp(in->src2.kind)) {
                int id = varmap_operand_id(&liv->varMap, in->src2);
                if (id >= 0) liveset_set(&live, id);
            }
            if (def && dstId >= 0) {
                liveset_clrbit(&live, dstId);
            }
        }
    }
}

/* ---- PASSO 6: Sweep (compatta array istruzioni e aggiorna blocchi) -----
 *   Questa funzione è identica a cp_sweep() in cp.c.
 *   Se si volesse riusarla, basterà spostarla in un header condiviso.
 * ----------------------------------------------------------------------- */
static int dce_sweep(IRFunction *f, char *eliminate, int nBlocks) {
    int nInstrs = f->count;
    IRInstr *newInstrs = malloc((size_t)nInstrs * sizeof(IRInstr));
    int newCount = 0;

    for (int b = 0; b < nBlocks; b++) {
        int oldStart = f->blocks[b].bb.start;
        int oldEnd   = f->blocks[b].bb.end;
        int newStart = newCount;

        for (int i = oldStart; i < oldEnd; i++) {
            if (!eliminate[i])
                newInstrs[newCount++] = f->instrs[i];
        }

        f->blocks[b].bb.start = newStart;
        f->blocks[b].bb.end   = newCount;
    }

    free(f->instrs);
    f->instrs        = newInstrs;
    f->count         = newCount;
    f->capacity      = newCount;
    f->curBlockStart = 0;

    return newCount != nInstrs;
}

/* ---- dce_optimize: entry point ----------------------------------------- */
int dce_optimize(IRFunction *f) {
    if (!f || f->blockCount == 0 || f->count == 0) return 0;

    int nBlocks = f->blockCount;
    Arena *arena = arena_create(0);

    /* PASSO 0: Reachability */
    char *reachable = arena_alloc(arena, (size_t)nBlocks);
    memset(reachable, 0, (size_t)nBlocks);
    markReachableBlocks(f, reachable);

    /* PASSO 1-4: Liveness (fronte IR) */
    LivenessResult liv = liveness_compute_ir(f, reachable, arena);

    /* PASSO 5: Mark */
    char *eliminate = arena_alloc(arena, (size_t)f->count);
    dce_mark(f, reachable, &liv, arena, eliminate);

    /* PASSO 6: Sweep */
    int changed = dce_sweep(f, eliminate, nBlocks);

    /* PASSO 7: Pulizia */
    arena_destroy(arena);
    varmap_destroy(&liv.varMap);

    return changed;
}