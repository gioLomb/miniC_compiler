#include <stdlib.h>
#include <string.h>
#include "sr.h"
#include "loop.h"
#include "liveness.h"
#include "arena.h"

/* ---- Costante folding intero (riusato da cp.c) - */
static int foldInt(IROp op, int a, int b, int *res) {
    switch (op) {
    case IR_MUL: *res = a * b; return 1;
    case IR_ADD: *res = a + b; return 1;
    case IR_SUB: *res = a - b; return 1;
    default: return 0;
    }
}

/* ---- Strutture dati ---------------------------------------------------- */
typedef struct {
    Operand var;
    int     varId;
    int     step;
    int     incrInstr;
} InductionBase;

typedef struct {
    int     mulInstr;
    int     dstId;
    Operand dst;
    int     multiplier;
    int     stride;
    int     srTempId;
} InductionDerived;

#define MAX_IVARS    16
#define MAX_DERIVED  64

/* ---- Helpers ----------------------------------------------------------- */
static int sameOperand(Operand a, Operand b) {
    if (a.kind != b.kind) return 0;
    if (a.kind == OPND_VAR)
        return a.data.varLevel == b.data.varLevel &&
               a.data.varOffset == b.data.varOffset;
    if (a.kind == OPND_TEMP)
        return a.data.tempId == b.data.tempId;
    return 0;
}

/* ---- Passo 1: trova le induttive base ---------------------------------- */
static int findInductionBase(IRFunction *f, Loop *L, VarMap *vm,
                              InductionBase *ivars, Arena *arena) {
    int count = 0;
    int numVars = vm->nextId;
    int *defCount = arena_alloc(arena, (size_t)numVars * sizeof(int));
    memset(defCount, 0, (size_t)numVars * sizeof(int));
    for (int i = 0; i < L->bodyCount; i++) {
        int b = L->body[i];
        for (int j = f->blocks[b].start; j < f->blocks[b].end; j++) {
            IRInstr *in = &f->instrs[j];
            if (!liveness_defines_dst(in->op)) continue;
            int id = varmap_operand_id(vm, in->dst);
            if (id >= 0) defCount[id]++;
        }
    }

    for (int i = 0; i < L->bodyCount && count < MAX_IVARS; i++) {
        int b = L->body[i];
        for (int j = f->blocks[b].start; j < f->blocks[b].end; j++) {
            IRInstr *in = &f->instrs[j];
            if (in->op != IR_ADD && in->op != IR_SUB) continue;
            if (!liveness_is_var_or_temp(in->dst.kind)) continue;
            if (!sameOperand(in->dst, in->src1)) continue;
            if (in->src2.kind != OPND_CONST_INT) continue;

            int id = varmap_operand_id(vm, in->dst);
            if (id < 0 || defCount[id] != 1) continue;

            int step = in->src2.data.intVal;
            if (in->op == IR_SUB) step = -step;

            InductionBase *iv = &ivars[count++];
            iv->var       = in->dst;
            iv->varId     = id;
            iv->step      = step;
            iv->incrInstr = j;
        }
    }
    return count;
}

/* ---- Passo 2: trova le derivate ---------------------------------------- */
static int findDerived(IRFunction *f, Loop *L, VarMap *vm,
                        InductionBase *ivars, int ivarCount,
                        InductionDerived *derived, int *nextTemp) {
    int count = 0;
    for (int i = 0; i < L->bodyCount; i++) {
        int b = L->body[i];
        for (int j = f->blocks[b].start; j < f->blocks[b].end && count < MAX_DERIVED; j++) {
            IRInstr *in = &f->instrs[j];
            if (in->op != IR_MUL) continue;
            if (!liveness_is_var_or_temp(in->dst.kind)) continue;

            for (int v = 0; v < ivarCount; v++) {
                InductionBase *iv = &ivars[v];
                int d = 0;
                if (sameOperand(in->src1, iv->var) && in->src2.kind == OPND_CONST_INT) {
                    d = in->src2.data.intVal;
                } else if (sameOperand(in->src2, iv->var) && in->src1.kind == OPND_CONST_INT) {
                    d = in->src1.data.intVal;
                } else {
                    continue;
                }

                int stride;
                if (!foldInt(IR_MUL, iv->step, d, &stride)) continue;

                int dstId = varmap_operand_id(vm, in->dst);
                if (dstId < 0) continue;

                InductionDerived *der = &derived[count++];
                der->mulInstr   = j;
                der->dstId      = dstId;
                der->dst        = in->dst;
                der->multiplier = d;
                der->stride     = stride;
                der->srTempId   = (*nextTemp)++;
                break;
            }
        }
    }
    return count;
}

/* ---- Passo 3: trasformazione (corretto) ------------------------------- */
static int applyStrengthReduction(IRFunction *f, Loop *L,
                                   InductionBase *ivars, int ivarCount,
                                   InductionDerived *derived, int derivedCount) {
    if (derivedCount == 0) return 0;

    int phIdx = L->preHeader;
    int nInstrs = f->count;
    int nBlocks = f->blockCount;
    int extra = derivedCount * 2; // init + update

    IRInstr *newInstrs = malloc((size_t)(nInstrs + extra) * sizeof(IRInstr));
    int newCount = 0;

    int *old_to_new = calloc((size_t)nInstrs, sizeof(int));
    for (int i = 0; i < nInstrs; i++) old_to_new[i] = -1;

    int *block_updates = calloc((size_t)nBlocks, sizeof(int));
    int phStart = f->blocks[phIdx].start;
    int phEnd   = f->blocks[phIdx].end;

    /* ---- Copia le istruzioni originali del pre-header ---- */
    for (int j = phStart; j < phEnd; j++) {
        old_to_new[j] = newCount;
        newInstrs[newCount++] = f->instrs[j];
    }

    /* ---- Inserisci init SR ---- */
    int initStart = newCount;
    for (int d = 0; d < derivedCount; d++) {
        InductionDerived *der = &derived[d];
        InductionBase *iv = NULL;
        for (int v = 0; v < ivarCount; v++) {
            IRInstr *orig = &f->instrs[der->mulInstr];
            if (sameOperand(ivars[v].var, orig->src1) ||
                sameOperand(ivars[v].var, orig->src2)) {
                iv = &ivars[v];
                break;
            }
        }
        if (!iv) continue;

        /* Prova a trovare il valore iniziale di iv->var nel pre-header */
        int initVal = 0;
        int hasInit = 0;
        for (int j = phStart; j < phEnd; j++) {
            IRInstr *in = &f->instrs[j];
            if (in->op == IR_ASSIGN && sameOperand(in->dst, iv->var) && in->src1.kind == OPND_CONST_INT) {
                initVal = in->src1.data.intVal;
                hasInit = 1;
                break;
            }
        }
        /* Se il pre-header è vuoto, assumiamo che il valore iniziale sia 0 (caso comune) */
        if (!hasInit && phStart == phEnd) {
            hasInit = 1;
            initVal = 0;
        }

        IRInstr init;
        memset(&init, 0, sizeof(init));
        if (hasInit) {
            int initialProduct;
            if (foldInt(IR_MUL, initVal, der->multiplier, &initialProduct)) {
                init.op = IR_ASSIGN;
                init.dst.kind = OPND_TEMP; init.dst.data.tempId = der->srTempId;
                init.src1.kind = OPND_CONST_INT; init.src1.data.intVal = initialProduct;
            } else {
                init.op = IR_MUL;
                init.dst.kind = OPND_TEMP; init.dst.data.tempId = der->srTempId;
                init.src1 = iv->var;
                init.src2.kind = OPND_CONST_INT; init.src2.data.intVal = der->multiplier;
            }
        } else {
            init.op = IR_MUL;
            init.dst.kind = OPND_TEMP; init.dst.data.tempId = der->srTempId;
            init.src1 = iv->var;
            init.src2.kind = OPND_CONST_INT; init.src2.data.intVal = der->multiplier;
        }
        newInstrs[newCount++] = init;
    }
    int initEnd = newCount;

    /* ---- Copia il resto e inserisci update ---- */
    for (int j = 0; j < nInstrs; j++) {
        if (j >= phStart && j < phEnd) continue;

        IRInstr *in = &f->instrs[j];
        int blk = -1;
        for (int b = 0; b < nBlocks; b++) {
            if (j >= f->blocks[b].start && j < f->blocks[b].end) { blk = b; break; }
        }
        if (blk < 0) blk = 0;

        int isDerivedMul = 0;
        for (int d = 0; d < derivedCount; d++) {
            if (j == derived[d].mulInstr) {
                IRInstr copy = {0};
                copy.op       = IR_ASSIGN;
                copy.dst      = derived[d].dst;
                copy.src1.kind = OPND_TEMP;
                copy.src1.data.tempId = derived[d].srTempId;
                copy.src2.kind = OPND_NONE;
                old_to_new[j] = newCount;
                newInstrs[newCount++] = copy;
                isDerivedMul = 1;
                break;
            }
        }
        if (isDerivedMul) continue;

        old_to_new[j] = newCount;
        newInstrs[newCount++] = *in;

        for (int v = 0; v < ivarCount; v++) {
            if (j == ivars[v].incrInstr) {
                int updates_for_this_incr = 0;
                for (int d = 0; d < derivedCount; d++) {
                    IRInstr *orig = &f->instrs[derived[d].mulInstr];
                    if (!sameOperand(ivars[v].var, orig->src1) &&
                        !sameOperand(ivars[v].var, orig->src2)) continue;
                    IRInstr upd = {0};
                    upd.op        = IR_ADD;
                    upd.dst.kind = OPND_TEMP; upd.dst.data.tempId = derived[d].srTempId;
                    upd.src1.kind = OPND_TEMP; upd.src1.data.tempId = derived[d].srTempId;
                    upd.src2.kind = OPND_CONST_INT; upd.src2.data.intVal = derived[d].stride;
                    newInstrs[newCount++] = upd;
                    updates_for_this_incr++;
                }
                block_updates[blk] += updates_for_this_incr;
            }
        }
    }

    free(f->instrs);
    f->instrs = newInstrs;
    f->count = newCount;
    f->capacity = newCount;

    /* ---- Aggiorna start/end ---- */
    for (int b = 0; b < nBlocks; b++) {
        int oldS = f->blocks[b].start, oldE = f->blocks[b].end;
        int newS = -1, newE = -1;

        if (b == phIdx) {
            int orig_start = -1, orig_end = -1;
            for (int j = oldS; j < oldE; j++) {
                if (old_to_new[j] != -1) {
                    if (orig_start == -1) orig_start = old_to_new[j];
                    orig_end = old_to_new[j] + 1;
                }
            }
            if (orig_start == -1) {
                newS = initStart;
                newE = initEnd;
            } else {
                newS = orig_start;
                newE = initEnd;
            }
        } else {
            int first = -1, last_orig = -1;
            for (int j = oldS; j < oldE; j++) {
                if (old_to_new[j] != -1) {
                    if (first == -1) first = old_to_new[j];
                    last_orig = old_to_new[j];
                }
            }
            if (first == -1) {
                newS = 0; newE = 0;
            } else {
                newS = first;
                newE = last_orig + 1 + block_updates[b];
            }
        }
        f->blocks[b].start = newS;
        f->blocks[b].end   = newE;
    }

    f->curBlockStart = 0;
    free(old_to_new);
    free(block_updates);
    return 1;
}

/* ---- Punto di ingresso ------------------------------------------------- */
int sr_optimize(IRFunction *f) {
    if (!f || f->blockCount == 0 || f->count == 0) return 0;

    int nBlocks = f->blockCount;
    int words   = (nBlocks + 63) / 64;
    Arena *arena = arena_create(0);

    LiveSet *Dom = loop_compute_dominators(f, words, arena);
    Loop *loops  = arena_alloc(arena, MAX_LOOPS * sizeof(Loop));
    int nLoops   = loop_find(f, Dom, loops, arena);
    if (nLoops == 0) { arena_destroy(arena); return 0; }

    Arena *livArena = arena_create(0);
    LivenessResult liv = liveness_compute(f, NULL, livArena);
    VarMap *vm = &liv.varMap;

    int nextTemp = 0;
    for (int i = 0; i < f->count; i++) {
        if (f->instrs[i].dst.kind  == OPND_TEMP && f->instrs[i].dst.data.tempId  >= nextTemp)
            nextTemp = f->instrs[i].dst.data.tempId + 1;
        if (f->instrs[i].src1.kind == OPND_TEMP && f->instrs[i].src1.data.tempId >= nextTemp)
            nextTemp = f->instrs[i].src1.data.tempId + 1;
        if (f->instrs[i].src2.kind == OPND_TEMP && f->instrs[i].src2.data.tempId >= nextTemp)
            nextTemp = f->instrs[i].src2.data.tempId + 1;
    }

    int totalChanged = 0;
    for (int l = 0; l < nLoops; l++) {
        Loop *L = &loops[l];
        if (L->preHeader < 0) loop_build_pre_header(f, L);

        InductionBase   ivars[MAX_IVARS];
        InductionDerived derived[MAX_DERIVED];

        int ivarCount = findInductionBase(f, L, vm, ivars, arena);
        if (ivarCount == 0) continue;

        int derivedCount = findDerived(f, L, vm, ivars, ivarCount, derived, &nextTemp);
        if (derivedCount == 0) continue;

        int changed = applyStrengthReduction(f, L, ivars, ivarCount, derived, derivedCount);
        totalChanged += changed;
    }

    varmap_destroy(&liv.varMap);
    arena_destroy(livArena);
    arena_destroy(arena);
    return totalChanged > 0;
}