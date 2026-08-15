#include <stdlib.h>
#include <string.h>
#include "cp.h"
#include "arena.h"
#include "varmap.h"
#include "constmap.h"

/* ---- Helpers per la fase di riscrittura -------------------------------- */
static inline int operand_equal(const Operand *a, const Operand *b) {
    if (a->kind != b->kind) return 0;
    if (a->kind == OPND_CONST_INT)  return a->data.intVal == b->data.intVal;
    if (a->kind == OPND_CONST_FLOAT) return a->data.floatVal == b->data.floatVal;
    if (a->kind == OPND_VAR) {
        return a->data.varLevel == b->data.varLevel &&
               a->data.varOffset == b->data.varOffset;
    }
    if (a->kind == OPND_TEMP) return a->data.tempId == b->data.tempId;
    if (a->kind == OPND_LABEL) return a->data.labelId == b->data.labelId;
    if (a->kind == OPND_FUNC) return strcmp(a->data.funcName, b->data.funcName) == 0;
    return 1; /* OPND_NONE */
}

/* ---- transferInstr: aggiorna la ConstMap con l'istruzione data --------- */
static void transferInstr(const IRInstr *in, ConstMap *map, VarMap *vm) {
    int id = varmap_operand_id(vm, in->dst);
    if (id < 0 || id >= map->size) return;

    LatVal result = lat_conflict();

    if (in->op == IR_ASSIGN) {
        result = getLatVal(map, in->src1, vm);
    } else if (isBinaryOp(in->op)) {
        LatVal lhs = getLatVal(map, in->src1, vm);
        LatVal rhs = getLatVal(map, in->src2, vm);
        if (lhs.state == LAT_CONST && rhs.state == LAT_CONST) {
            if (!lhs.isFloat && !rhs.isFloat) {
                int r;
                if (foldBinaryInt(in->op, lhs.val.ival, rhs.val.ival, &r))
                    result = lat_const_int(r);
            } else if (lhs.isFloat && rhs.isFloat) {
                float r;
                if (foldBinaryFloat(in->op, lhs.val.fval, rhs.val.fval, &r))
                    result = isComparisonOp(in->op)
                             ? lat_const_int((int)r)
                             : lat_const_float(r);
            }
        }
    } else if (in->op == IR_NEG || in->op == IR_NOT) {
        result = foldUnary(in->op, getLatVal(map, in->src1, vm));
    }

    map->vals[id] = result;
}

/* ---- PASSO 1: Build VarMap --------------------------------------------- */
static void cp_build_varmap(IRFunction *f, VarMap *vm) {
    varmap_init(vm);
    for (int i = 0; i < f->count; i++) {
        IRInstr *in = &f->instrs[i];
        varmap_operand_id(vm, in->dst);
        varmap_operand_id(vm, in->src1);
        varmap_operand_id(vm, in->src2);
    }
}

/* ---- PASSO 3: Forward dataflow ----------------------------------------- */
static void cp_run_dataflow(IRFunction *f, ConstMap *In, ConstMap *Out,
                             ConstMap *tmp, int numVars, VarMap *vm) {
    (void)numVars;
    int nBlocks = f->blockCount;
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int b = 0; b < nBlocks; b++) {
            int hasPred = 0;
            for (int p = 0; p < nBlocks; p++) {
                for (int k = 0; k < 2; k++) {
                    if (f->blocks[p].bb.succ[k] != b) continue;
                    if (!hasPred) {
                        constMap_copy(&In[b], &Out[p]);
                        hasPred = 1;
                    } else {
                        constMap_meet(&In[b], &Out[p]);
                    }
                }
            }

            constMap_copy(tmp, &In[b]);
            for (int i = f->blocks[b].bb.start; i < f->blocks[b].bb.end; i++)
                transferInstr(&f->instrs[i], tmp, vm);

            if (!constMap_equal(&Out[b], tmp)) {
                constMap_copy(&Out[b], tmp);
                changed = 1;
            }
        }
    }
}

/* ---- Folding binario costante (usato nella riscrittura) ----------------
 *
 * Gestisce la promozione int→float qui (responsabilità locale), poi
 * delega il kernel aritmetico a foldBinaryInt/foldBinaryFloat di constmap.
 * Lo switch sui casi dell'operazione vive solo in constmap.c.
 * ----------------------------------------------------------------------- */
static int cp_fold_binary_instr(IRInstr *in, const Operand *ns1, const Operand *ns2) {
    int floatOp = (ns1->kind == OPND_CONST_FLOAT || ns2->kind == OPND_CONST_FLOAT);
    IROp origOp = in->op;

    if (floatOp) {
        float a = (ns1->kind == OPND_CONST_INT) ? (float)ns1->data.intVal
                                                 : ns1->data.floatVal;
        float b = (ns2->kind == OPND_CONST_INT) ? (float)ns2->data.intVal
                                                 : ns2->data.floatVal;
        float result;
        if (!foldBinaryFloat(origOp, a, b, &result)) return 0;

        in->op   = IR_ASSIGN;
        in->src2 = noOperand();
        /* comparazioni float → risultato int (0/1) */
        if (isComparisonOp(origOp)) {
            in->src1 = (Operand){ .kind = OPND_CONST_INT,
                                  .data.intVal = (int)result };
        } else {
            in->src1 = (Operand){ .kind = OPND_CONST_FLOAT,
                                  .data.floatVal = result };
        }
    } else {
        int a = ns1->data.intVal;
        int b = ns2->data.intVal;
        int result;
        if (!foldBinaryInt(origOp, a, b, &result)) return 0;

        in->op   = IR_ASSIGN;
        in->src1 = (Operand){ .kind = OPND_CONST_INT,
                              .data.intVal = result };
        in->src2 = noOperand();
    }
    return 1;
}

/* ---- Verifica se un salto punta all'istruzione successiva ------------- */
static int is_jump_to_next(IRFunction *f, int idx) {
    if (idx + 1 >= f->count) return 0;
    IRInstr *in = &f->instrs[idx];
    if (in->op != IR_GOTO && in->op != IR_IF_FALSE) return 0;
    int labelId = in->dst.data.labelId;
    for (int b = 0; b < f->blockCount; b++) {
        int start = f->blocks[b].bb.start;
        int end   = f->blocks[b].bb.end;
        if (start <= idx + 1 && idx + 1 < end) {
            if (f->instrs[idx + 1].op == IR_LABEL &&
                f->instrs[idx + 1].dst.data.labelId == labelId) {
                return 1;
            }
        }
    }
    return 0;
}

/* ---- Marca le label orfane (senza riferimenti) ------------------------- */
static int mark_unreferenced_labels(IRFunction *f, char *eliminate) {
    int count = 0;
    char *referenced = calloc((size_t)f->count, 1);
    for (int i = 0; i < f->count; i++) {
        if (f->instrs[i].op == IR_GOTO || f->instrs[i].op == IR_IF_FALSE) {
            int labelId = f->instrs[i].dst.data.labelId;
            for (int j = 0; j < f->count; j++) {
                if (f->instrs[j].op == IR_LABEL &&
                    f->instrs[j].dst.data.labelId == labelId) {
                    referenced[j] = 1;
                    break;
                }
            }
        }
    }
    for (int i = 0; i < f->count; i++) {
        if (f->instrs[i].op == IR_LABEL && !referenced[i]) {
            eliminate[i] = 1;
            count++;
        }
    }
    free(referenced);
    return count;
}

/* ---- PASSO 4: Riscrittura singolo blocco ------------------------------- */
static int cp_rewrite_block(IRFunction *f, int b, ConstMap *In,
                             char *eliminate, VarMap *vm, Arena *arena) {
    int numVars = vm->nextId;
    int modified = 0;

    ConstMap live;
    constMap_init(&live, numVars, arena);
    constMap_copy(&live, &In[b]);

    for (int i = f->blocks[b].bb.start; i < f->blocks[b].bb.end; i++) {
        IRInstr *in = &f->instrs[i];

        /* ---- Jump-to-next elimination ---- */
        if ((in->op == IR_GOTO || in->op == IR_IF_FALSE) &&
            is_jump_to_next(f, i)) {
            int s = (in->op == IR_GOTO)
                    ? f->blocks[b].bb.succ[0]
                    : f->blocks[b].bb.succ[1];
            if (s >= 0) f->blocks[s].predCount--;
            if (in->op == IR_GOTO) {
                f->blocks[b].bb.succ[0] = f->blocks[b].bb.succ[1];
                f->blocks[b].bb.succ[1] = -1;
            } else {
                f->blocks[b].bb.succ[1] = -1;
            }
            eliminate[i] = 1;
            modified = 1;
            transferInstr(in, &live, vm);
            continue;
        }

        /* ---- IF_FALSE folding ---- */
        if (in->op == IR_IF_FALSE) {
            Operand cond = constMap_try_fold(in->src1, &live, vm);
            if (cond.kind == OPND_CONST_INT || cond.kind == OPND_CONST_FLOAT) {
                int isZero = (cond.kind == OPND_CONST_INT)
                             ? (cond.data.intVal == 0)
                             : (cond.data.floatVal == 0.0f);
                int taken = isZero;
                int s0 = f->blocks[b].bb.succ[0];
                int s1 = f->blocks[b].bb.succ[1];
                if (taken) {
                    in->op   = IR_GOTO;
                    in->src1 = in->src2 = (Operand){.kind = OPND_NONE};
                    f->blocks[b].bb.succ[0] = s1;
                    f->blocks[b].bb.succ[1] = -1;
                    if (s0 >= 0) f->blocks[s0].predCount--;
                } else {
                    eliminate[i] = 1;
                    f->blocks[b].bb.succ[1] = -1;
                    if (s1 >= 0) f->blocks[s1].predCount--;
                }
                modified = 1;
            } else if (!operand_equal(&cond, &in->src1)) {
                in->src1 = cond;
                modified = 1;
            }
            transferInstr(in, &live, vm);
            continue;
        }

        /* ---- Sostituisci operandi con costanti note ---- */
        Operand ns1 = constMap_try_fold(in->src1, &live, vm);
        Operand ns2 = constMap_try_fold(in->src2, &live, vm);
        if (!operand_equal(&ns1, &in->src1)) { in->src1 = ns1; modified = 1; }
        if (!operand_equal(&ns2, &in->src2)) { in->src2 = ns2; modified = 1; }

        /* ---- Folding binario ---- */
        if (in->op != IR_IF_FALSE && in->op != IR_LABEL &&
            in->op != IR_GOTO     && in->op != IR_RETURN &&
            (in->op == IR_ADD || in->op == IR_SUB || in->op == IR_MUL ||
             in->op == IR_DIV || in->op == IR_MOD ||
             in->op == IR_LT  || in->op == IR_LE  || in->op == IR_GT  ||
             in->op == IR_GE  || in->op == IR_EQ  || in->op == IR_NE)) {

            if ((ns1.kind == OPND_CONST_INT || ns1.kind == OPND_CONST_FLOAT) &&
                (ns2.kind == OPND_CONST_INT || ns2.kind == OPND_CONST_FLOAT)) {

                modified |= cp_fold_binary_instr(in, &ns1, &ns2);
            }
        }

        transferInstr(in, &live, vm);
    }

    return modified;
}

/* ---- PASSO 5: Sweep (compatta le istruzioni) --------------------------- */
static int cp_sweep(IRFunction *f, char *eliminate, int nBlocks) {
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

    return (newCount != nInstrs) ? 1 : 0;
}

/* ---- cp_optimize: entry point ------------------------------------------ */
int cp_optimize(IRFunction *f) {
    if (!f || f->blockCount == 0 || f->count == 0) return 0;

    int nBlocks = f->blockCount;
    Arena *arena = arena_create(0);

    /* PASSO 1: Build VarMap */
    VarMap vm;
    cp_build_varmap(f, &vm);
    int numVars = vm.nextId;

    /* PASSO 2: Alloca In/Out */
    ConstMap *In  = arena_alloc(arena, (size_t)nBlocks * sizeof(ConstMap));
    ConstMap *Out = arena_alloc(arena, (size_t)nBlocks * sizeof(ConstMap));
    ConstMap  tmp;
    constMap_init(&tmp, numVars, arena);
    for (int b = 0; b < nBlocks; b++) {
        constMap_init(&In[b],  numVars, arena);
        constMap_init(&Out[b], numVars, arena);
    }

    /* PASSO 3: Forward dataflow */
    cp_run_dataflow(f, In, Out, &tmp, numVars, &vm);

    /* PASSO 4: Riscrittura + CFG pruning + folding */
    int modified = 0;
    char *eliminate = arena_alloc(arena, (size_t)f->count * sizeof(char));
    memset(eliminate, 0, (size_t)f->count * sizeof(char));

    for (int b = 0; b < nBlocks; b++) {
        modified |= cp_rewrite_block(f, b, In, eliminate, &vm, arena);
    }

    /* Rimozione label orfane */
    modified |= (mark_unreferenced_labels(f, eliminate) > 0);

    /* PASSO 5: Sweep */
    if (modified) {
        modified = cp_sweep(f, eliminate, nBlocks);
    }

    arena_destroy(arena);
    varmap_destroy(&vm);
    return modified;
}