#include <stdlib.h>
#include <string.h>
#include "sr.h"
#include "loop.h"
#include "liveness.h"
#include "arena.h"

static int foldInt(IROp op, int a, int b, int *res) {
    switch (op) {
    case IR_MUL: *res = a * b; return 1;
    case IR_ADD: *res = a + b; return 1;
    case IR_SUB: *res = a - b; return 1;
    default: return 0;
    }
}

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

static int sameOperand(Operand a, Operand b) {
    if (a.kind != b.kind) return 0;
    if (a.kind == OPND_VAR)
        return a.data.varLevel == b.data.varLevel &&
               a.data.varOffset == b.data.varOffset;
    if (a.kind == OPND_TEMP)
        return a.data.tempId == b.data.tempId;
    return 0;
}

/* ---- Passo 1 ----------------------------------------------------------- */
static int findInductionBase(IRFunction *f, Loop *L, VarMap *vm,
                              InductionBase *ivars, Arena *arena) {
    int count   = 0;
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
            iv->var = in->dst; iv->varId = id;
            iv->step = step;   iv->incrInstr = j;
        }
    }
    return count;
}

/* ---- Passo 2 ----------------------------------------------------------- */
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
                if      (sameOperand(in->src1, iv->var) && in->src2.kind == OPND_CONST_INT) d = in->src2.data.intVal;
                else if (sameOperand(in->src2, iv->var) && in->src1.kind == OPND_CONST_INT) d = in->src1.data.intVal;
                else continue;
                int stride;
                if (!foldInt(IR_MUL, iv->step, d, &stride)) continue;
                int dstId = varmap_operand_id(vm, in->dst);
                if (dstId < 0) continue;
                InductionDerived *der = &derived[count++];
                der->mulInstr = j; der->dstId = dstId; der->dst = in->dst;
                der->multiplier = d; der->stride = stride;
                der->srTempId = (*nextTemp)++; der->baseIdx = v;
                break;
            }
        }
    }
    return count;
}

/* ---- Passo 3 -----------------------------------------------------------
 * Costruisce un nuovo array f->instrs inserendo:
 *   - le init SR (t_sr = i * d) PRIMA delle istruzioni dell'header del loop
 *     (posizione fisica corretta nell'IR lineare)
 *   - le copie t = t_sr al posto delle moltiplicazioni originali nel body
 *   - gli aggiornamenti t_sr = t_sr + stride dopo ogni incremento
 * Aggiorna start/end di tutti i blocchi tramite mappa indice.
 * Il blocco pre-header (phIdx) riceve il range delle init SR.
 * -------------------------------------------------------------------- */
static int applyStrengthReduction(IRFunction *f, Loop *L,
                                   InductionBase *ivars, int ivarCount,
                                   InductionDerived *derived, int derivedCount) {
    if (derivedCount == 0) return 0;

    int phIdx    = L->preHeader;
    int header   = L->header;
    int nInstrs  = f->count;
    int nBlocks  = f->blockCount;

    /* Punto di inserimento fisico delle init SR:
     * PRIMA della prima istruzione dell'header del loop. */
    int insertAt = f->blocks[header].start;

    int maxNew = nInstrs + derivedCount * (1 + ivarCount);
    IRInstr *newInstrs = malloc((size_t)maxNew * sizeof(IRInstr));
    int newCount = 0;

    /* map[j] = nuovo indice di f->instrs[j]; -1 se eliminato */
    int *map = malloc((size_t)nInstrs * sizeof(int));
    for (int i = 0; i < nInstrs; i++) map[i] = -1;

    int phInitStart = -1, phInitEnd = -1;

    for (int j = 0; j < nInstrs; j++) {
        /* Punto di inserimento: emetti le init SR */
        if (j == insertAt) {
            phInitStart = newCount;
            for (int d = 0; d < derivedCount; d++) {
                InductionDerived *der = &derived[d];
                InductionBase    *iv  = &ivars[der->baseIdx];
                IRInstr init; memset(&init, 0, sizeof init);
                init.op               = IR_MUL;
                init.dst.kind         = OPND_TEMP;
                init.dst.data.tempId  = der->srTempId;
                init.src1             = iv->var;
                init.src2.kind        = OPND_CONST_INT;
                init.src2.data.intVal = der->multiplier;
                newInstrs[newCount++] = init;
            }
            phInitEnd = newCount;
        }

        IRInstr *in = &f->instrs[j];

        /* t = i * d → t = t_sr */
        int isDerived = 0;
        for (int d = 0; d < derivedCount; d++) {
            if (j != derived[d].mulInstr) continue;
            IRInstr copy; memset(&copy, 0, sizeof copy);
            copy.op               = IR_ASSIGN;
            copy.dst              = derived[d].dst;
            copy.src1.kind        = OPND_TEMP;
            copy.src1.data.tempId = derived[d].srTempId;
            copy.src2.kind        = OPND_NONE;
            map[j] = newCount;
            newInstrs[newCount++] = copy;
            isDerived = 1;
            break;
        }
        if (!isDerived) {
            map[j] = newCount;
            newInstrs[newCount++] = *in;
        }

        /* Dopo i = i + c: inserisci t_sr = t_sr + stride */
        for (int v = 0; v < ivarCount; v++) {
            if (j != ivars[v].incrInstr) continue;
            for (int d = 0; d < derivedCount; d++) {
                if (derived[d].baseIdx != v) continue;
                IRInstr upd; memset(&upd, 0, sizeof upd);
                upd.op               = IR_ADD;
                upd.dst.kind         = OPND_TEMP;
                upd.dst.data.tempId  = derived[d].srTempId;
                upd.src1.kind        = OPND_TEMP;
                upd.src1.data.tempId = derived[d].srTempId;
                upd.src2.kind        = OPND_CONST_INT;
                upd.src2.data.intVal = derived[d].stride;
                newInstrs[newCount++] = upd;
            }
        }
    }

    /* Caso degenere: insertAt == nInstrs (header alla fine) */
    if (phInitStart == -1) {
        phInitStart = newCount;
        for (int d = 0; d < derivedCount; d++) {
            InductionDerived *der = &derived[d];
            InductionBase    *iv  = &ivars[der->baseIdx];
            IRInstr init; memset(&init, 0, sizeof init);
            init.op               = IR_MUL;
            init.dst.kind         = OPND_TEMP;
            init.dst.data.tempId  = der->srTempId;
            init.src1             = iv->var;
            init.src2.kind        = OPND_CONST_INT;
            init.src2.data.intVal = der->multiplier;
            newInstrs[newCount++] = init;
        }
        phInitEnd = newCount;
    }

    free(f->instrs);
    f->instrs   = newInstrs;
    f->count    = newCount;
    f->capacity = newCount;

    /* Aggiorna start/end di ogni blocco */
    for (int b = 0; b < nBlocks; b++) {
        if (b == phIdx) {
            /* Il pre-header corrisponde alle init SR */
            f->blocks[b].start = phInitStart;
            f->blocks[b].end   = phInitEnd;
            continue;
        }

        int oldS = f->blocks[b].start;
        int oldE = f->blocks[b].end;
        int newS = -1, newE = -1;
        for (int j = oldS; j < oldE; j++) {
            if (map[j] == -1) continue;
            if (newS == -1) newS = map[j];
            newE = map[j] + 1;
        }
        /* Estendi end per gli aggiornamenti SR inseriti nel body */
        for (int v = 0; v < ivarCount; v++) {
            int incrJ = ivars[v].incrInstr;
            if (incrJ < oldS || incrJ >= oldE) continue;
            for (int d = 0; d < derivedCount; d++)
                if (derived[d].baseIdx == v && newE >= 0) newE++;
        }
        /* Tutti i blocchi con start >= insertAt sono stati spostati
         * in avanti di derivedCount posizioni */
        if (newS == -1 && oldS >= insertAt) {
            newS = oldS + derivedCount;
            newE = oldE + derivedCount;
        }
        f->blocks[b].start = (newS == -1) ? 0 : newS;
        f->blocks[b].end   = (newE == -1) ? 0 : newE;
    }
    f->curBlockStart = 0;

    free(map);
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
        IRInstr *in = &f->instrs[i];
        if (in->dst.kind  == OPND_TEMP && in->dst.data.tempId  >= nextTemp) nextTemp = in->dst.data.tempId  + 1;
        if (in->src1.kind == OPND_TEMP && in->src1.data.tempId >= nextTemp) nextTemp = in->src1.data.tempId + 1;
        if (in->src2.kind == OPND_TEMP && in->src2.data.tempId >= nextTemp) nextTemp = in->src2.data.tempId + 1;
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
        totalChanged += applyStrengthReduction(f, L, ivars, ivarCount, derived, derivedCount);
    }

    varmap_destroy(&liv.varMap);
    arena_destroy(livArena);
    arena_destroy(arena);
    return totalChanged > 0;
}
