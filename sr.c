#include <stdlib.h>
#include <string.h>
#include "sr.h"
#include "loop.h"
#include "liveness.h"
#include "arena.h"

/* ---- Helpers ----------------------------------------------------------- */

static inline int foldInt(IROp op, int a, int b, int *res) {
    switch (op) {
    case IR_MUL: *res = a * b; return 1;
    case IR_ADD: *res = a + b; return 1;
    case IR_SUB: *res = a - b; return 1;
    default:                   return 0;
    }
}

static inline int sameOperand(Operand a, Operand b) {
    if (a.kind != b.kind) return 0;
    if (a.kind == OPND_VAR)
        return a.data.varLevel  == b.data.varLevel &&
               a.data.varOffset == b.data.varOffset;
    if (a.kind == OPND_TEMP)
        return a.data.tempId == b.data.tempId;
    return 0;
}

/* ---- Strutture --------------------------------------------------------- */

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
    int     baseIdx;
} InductionDerived;

#define MAX_IVARS   16
#define MAX_DERIVED 64

/* ---- Ricerca variabili induttive base ---------------------------------- */

static int findInductionBase(IRFunction *f, Loop *L, VarMap *vm,
                              InductionBase *ivars, Arena *arena) {
    int count   = 0;
    int numVars = vm->nextId;
    int *defCount = arena_alloc(arena, (size_t)numVars * sizeof(int));
    memset(defCount, 0, (size_t)numVars * sizeof(int));

    for (int i = 0; i < L->bodyCount; i++) {
        int b = L->body[i];
        for (int j = f->blocks[b].bb.start; j < f->blocks[b].bb.end; j++) {
            IRInstr *in = &f->instrs[j];
            if (!liveness_defines_dst(in->op)) continue;
            int id = varmap_operand_id(vm, in->dst);
            if (id >= 0) defCount[id]++;
        }
    }

    for (int i = 0; i < L->bodyCount && count < MAX_IVARS; i++) {
        int b = L->body[i];
        for (int j = f->blocks[b].bb.start;
             j < f->blocks[b].bb.end && count < MAX_IVARS;
             j++) {
            const IRInstr *in = &f->instrs[j];

            /* cerca pattern: dst = dst ± CONST, unica def nel loop */
            if (in->op != IR_ADD && in->op != IR_SUB)          continue;
            if (!liveness_is_var_or_temp(in->dst.kind))        continue;
            if (!sameOperand(in->dst, in->src1))               continue;
            if (in->src2.kind != OPND_CONST_INT)               continue;

            int id = varmap_operand_id(vm, in->dst);
            if (id < 0 || defCount[id] != 1)                   continue;

            int step = in->src2.data.intVal;
            if (in->op == IR_SUB) step = -step;

            ivars[count++] = (InductionBase){
                .var       = in->dst,
                .varId     = id,
                .step      = step,
                .incrInstr = j,
            };
        }
    }

    return count;
}

/* ---- Ricerca variabili induttive derivate ------------------------------ */

static int findDerived(IRFunction *f, Loop *L, VarMap *vm,
                        InductionBase *ivars, int ivarCount,
                        InductionDerived *derived, int *nextTemp) {
    int count = 0;

    for (int i = 0; i < L->bodyCount; i++) {
        int b = L->body[i];
        for (int j = f->blocks[b].bb.start;
             j < f->blocks[b].bb.end && count < MAX_DERIVED;
             j++) {
            const IRInstr *in = &f->instrs[j];

            if (in->op != IR_MUL)                              continue;
            if (!liveness_is_var_or_temp(in->dst.kind))        continue;

            for (int v = 0; v < ivarCount; v++) {
                InductionBase *iv = &ivars[v];
                int d = 0;

                if (sameOperand(in->src1, iv->var) && in->src2.kind == OPND_CONST_INT)
                    d = in->src2.data.intVal;
                else if (sameOperand(in->src2, iv->var) && in->src1.kind == OPND_CONST_INT)
                    d = in->src1.data.intVal;
                else
                    continue;

                int stride;
                if (!foldInt(IR_MUL, iv->step, d, &stride)) continue;

                int dstId = varmap_operand_id(vm, in->dst);
                if (dstId < 0) continue;

                derived[count++] = (InductionDerived){
                    .mulInstr   = j,
                    .dstId      = dstId,
                    .dst        = in->dst,
                    .multiplier = d,
                    .stride     = stride,
                    .srTempId   = (*nextTemp)++,
                    .baseIdx    = v,
                };
                break;
            }
        }
    }

    return count;
}

/* ---- Helper: costruisce una IRInstr azzerata con campi minimi ---------- */

static inline IRInstr make_instr(IROp op, Operand dst,
                                  Operand src1, Operand src2,
                                  int loopDepth) {
    return (IRInstr){ .op = op, .dst = dst,
                      .src1 = src1, .src2 = src2,
                      .loopDepth = loopDepth };
}

/* ---- Applicazione della strength reduction ----------------------------- */

static int applyStrengthReduction(IRFunction *f, Loop *L,
                                   InductionBase *ivars, int ivarCount,
                                   InductionDerived *derived, int derivedCount) {
    if (derivedCount == 0) return 0;

    int phIdx    = L->preHeader;
    int header   = L->header;
    int nInstrs  = f->count;
    int nBlocks  = f->blockCount;
    int insertAt = f->blocks[header].bb.start;

    /* loopDepth del corpo (prima istruzione dell'header) e dell'esterno */
    int bodyDepth  = f->instrs[insertAt].loopDepth;
    int outerDepth = bodyDepth > 0 ? bodyDepth - 1 : 0;

    int maxNew = nInstrs + derivedCount * (1 + ivarCount);
    IRInstr *newInstrs = malloc((size_t)maxNew * sizeof(IRInstr));
    int newCount = 0;

    int *oldToNew = malloc((size_t)nInstrs * sizeof(int));
    memset(oldToNew, -1, (size_t)nInstrs * sizeof(int));

    int phInitStart = -1, phInitEnd = -1;

    /* Emette l'init SR di tutte le derived variables nel pre-header */
    #define EMIT_PH_INITS()                                                  \
        do {                                                                  \
            phInitStart = newCount;                                           \
            for (int d = 0; d < derivedCount; d++) {                         \
                InductionDerived *der = &derived[d];                         \
                InductionBase    *iv  = &ivars[der->baseIdx];                \
                newInstrs[newCount++] = make_instr(                          \
                    IR_MUL,                                                   \
                    (Operand){ .kind = OPND_TEMP,                            \
                               .data.tempId = der->srTempId },               \
                    iv->var,                                                  \
                    (Operand){ .kind = OPND_CONST_INT,                       \
                               .data.intVal = der->multiplier },             \
                    outerDepth);                                              \
            }                                                                 \
            phInitEnd = newCount;                                             \
        } while (0)

    for (int j = 0; j < nInstrs; j++) {

        /* inserisci init SR prima dell'header (una sola volta) */
        if (j == insertAt) EMIT_PH_INITS();

        IRInstr *in = &f->instrs[j];

        /* t = i * d  →  t = t_sr */
        int isDerived = 0;
        for (int d = 0; d < derivedCount; d++) {
            if (j != derived[d].mulInstr) continue;
            oldToNew[j] = newCount;
            newInstrs[newCount++] = make_instr(
                IR_ASSIGN,
                derived[d].dst,
                (Operand){ .kind = OPND_TEMP,
                           .data.tempId = derived[d].srTempId },
                noOperand(),
                in->loopDepth);
            isDerived = 1;
            break;
        }
        if (!isDerived) {
            oldToNew[j] = newCount;
            newInstrs[newCount++] = *in;
        }

        /* dopo i = i + c: inserisci t_sr = t_sr + stride */
        for (int v = 0; v < ivarCount; v++) {
            if (j != ivars[v].incrInstr) continue;
            for (int d = 0; d < derivedCount; d++) {
                if (derived[d].baseIdx != v) continue;
                Operand srOp = (Operand){ .kind = OPND_TEMP,
                                          .data.tempId = derived[d].srTempId };
                newInstrs[newCount++] = make_instr(
                    IR_ADD, srOp, srOp,
                    (Operand){ .kind = OPND_CONST_INT,
                               .data.intVal = derived[d].stride },
                    bodyDepth);
            }
        }
    }

    /* caso degenere: insertAt == nInstrs (loop vuoto, non dovrebbe accadere
       in pratica, ma meglio gestirlo) */
    if (phInitStart == -1) EMIT_PH_INITS();

    #undef EMIT_PH_INITS

    free(f->instrs);
    f->instrs   = newInstrs;
    f->count    = newCount;
    f->capacity = newCount;

    /* Aggiorna start/end dei blocchi usando oldToNew[] per i blocchi body
     * e phInitStart/phInitEnd per il pre-header */
    for (int b = 0; b < nBlocks; b++) {
        if (b == phIdx) {
            f->blocks[b].bb.start = phInitStart;
            f->blocks[b].bb.end   = phInitEnd;
            continue;
        }
        int oldS = f->blocks[b].bb.start, oldE = f->blocks[b].bb.end;
        int newS = -1, newE = -1;
        for (int j = oldS; j < oldE; j++) {
            if (oldToNew[j] == -1) continue;
            if (newS == -1) newS = oldToNew[j];
            newE = oldToNew[j] + 1;
        }
        /* fallback per blocchi body che iniziano dopo insertAt senza map */
        if (newS == -1 && oldS >= insertAt) {
            newS = oldS + derivedCount;
            newE = oldE + derivedCount;
        }
        f->blocks[b].bb.start = (newS == -1) ? 0 : newS;
        f->blocks[b].bb.end   = (newE == -1) ? 0 : newE;
    }
    f->curBlockStart = 0;
    free(oldToNew);
    return 1;
}

/* ---- Punto di ingresso ------------------------------------------------- */

int sr_optimize(IRFunction *f) {
    if (!f || f->blockCount == 0 || f->count == 0) return 0;

    int nBlocks = f->blockCount;
    int words   = (nBlocks + BITS_PER_WORD-1) / BITS_PER_WORD;
    Arena *arena = arena_create(0);

    LiveSet *Dom  = loop_compute_dominators(f, words, arena);
    Loop    *loops  = arena_alloc(arena, MAX_LOOPS * sizeof(Loop));
    int      nLoops = loop_find(f, Dom, loops, arena);
    if (nLoops == 0) { arena_destroy(arena); return 0; }

    Arena         *livArena = arena_create(0);
    LivenessResult  liv     = liveness_compute_ir(f, NULL, livArena);
    VarMap         *vm      = &liv.varMap;

    /* calcola nextTemp in un unico scan su tutte le istruzioni */
    int nextTemp = 0;
    for (int i = 0; i < f->count; i++) {
        const IRInstr *in = &f->instrs[i];
        const Operand *ops[3] = { &in->dst, &in->src1, &in->src2 };
        for (int k = 0; k < 3; k++) {
            if (ops[k]->kind == OPND_TEMP && ops[k]->data.tempId >= nextTemp)
                nextTemp = ops[k]->data.tempId + 1;
        }
    }

    int totalChanged = 0;
    for (int l = 0; l < nLoops; l++) {
        Loop *L = &loops[l];
        if (L->preHeader < 0) loop_build_pre_header(f, L);

        InductionBase    ivars[MAX_IVARS];
        InductionDerived derived[MAX_DERIVED];

        int ivarCount    = findInductionBase(f, L, vm, ivars, arena);
        if (ivarCount == 0) continue;

        int derivedCount = findDerived(f, L, vm, ivars, ivarCount, derived, &nextTemp);
        if (derivedCount == 0) continue;

        totalChanged += applyStrengthReduction(f, L, ivars, ivarCount,
                                                derived, derivedCount);
    }

    varmap_destroy(&liv.varMap);
    arena_destroy(livArena);
    arena_destroy(arena);
    return totalChanged > 0;
}