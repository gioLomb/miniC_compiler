#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include "cp.h"
#include "arena.h"

/* ---- Abilita debug ---- */
#define CP_DEBUG 0

/* ---- Reticolo dei valori ----------------------------------------------- */
#define LAT_UNKNOWN  0
#define LAT_CONST    1
#define LAT_CONFLICT 2

typedef struct {
    int state;
    int isFloat;
    union {
        int   ival;
        float fval;
    } val;
} LatVal;

#include "hash_table.h"

static unsigned long cp_uint64_hash(const void *key, size_t keySize) {
    (void)keySize;
    uint64_t v = *(const uint64_t *)key;
    v ^= v >> 33; v *= 0xff51afd7ed558ccdULL;
    v ^= v >> 33; v *= 0xc4ceb9fe1a85ec53ULL;
    v ^= v >> 33;
    return (unsigned long)v;
}

typedef struct { Hash_Table *table; int nextId; } VarMap;

static inline uint64_t cp_make_key(int kind, int a, int b) {
    uint64_t k = 0;
    k |= (uint64_t)(kind & 0x3)         << 62;
    k |= (uint64_t)(a    & 0x7fffffff)  << 31;
    k |= (uint64_t)(b    & 0x7fffffff);
    return k;
}

static int varMap_id(VarMap *m, int kind, int a, int b) {
    uint64_t key = cp_make_key(kind, a, b);
    int id;
    if (ht_get(m->table, &key, sizeof key, &id, sizeof id)) return id;
    id = m->nextId++;
    ht_set(m->table, &key, sizeof key, &id, sizeof id);
    return id;
}

static inline int operandVarId(VarMap *m, Operand op) {
    if (op.kind == OPND_VAR)  return varMap_id(m, 0, op.data.varLevel, op.data.varOffset);
    if (op.kind == OPND_TEMP) return varMap_id(m, 1, op.data.tempId, 0);
    return -1;
}

/* ---- Confronto semantico tra Operand ---------------------------------- */
static inline int operand_equal(const Operand *a, const Operand *b) {
    if (a->kind != b->kind) return 0;
    switch (a->kind) {
    case OPND_NONE:        return 1;
    case OPND_TEMP:        return a->data.tempId   == b->data.tempId;
    case OPND_CONST_INT:   return a->data.intVal   == b->data.intVal;
    case OPND_CONST_FLOAT: return a->data.floatVal == b->data.floatVal;
    case OPND_LABEL:       return a->data.labelId  == b->data.labelId;
    case OPND_FUNC:        return a->data.funcName == b->data.funcName;
    case OPND_VAR:
        return a->data.varLevel  == b->data.varLevel &&
               a->data.varOffset == b->data.varOffset;
    default: return 0;
    }
}

/* ---- Operazioni sul reticolo ------------------------------------------ */

static inline LatVal lat_unknown(void) {
    LatVal v = {0}; v.state = LAT_UNKNOWN; return v;
}
static inline LatVal lat_const_int(int ival) {
    LatVal v = {0}; v.state = LAT_CONST; v.isFloat = 0; v.val.ival = ival; return v;
}
static inline LatVal lat_const_float(float fval) {
    LatVal v = {0}; v.state = LAT_CONST; v.isFloat = 1; v.val.fval = fval; return v;
}
static inline LatVal lat_conflict(void) {
    LatVal v = {0}; v.state = LAT_CONFLICT; return v;
}

static inline LatVal lat_meet(LatVal a, LatVal b) {
    if (a.state == LAT_UNKNOWN) return b;
    if (b.state == LAT_UNKNOWN) return a;
    if (a.state == LAT_CONFLICT || b.state == LAT_CONFLICT) return lat_conflict();
    if (a.isFloat != b.isFloat) return lat_conflict();
    if (a.isFloat) {
        if (a.val.fval == b.val.fval) return a;
    } else {
        if (a.val.ival == b.val.ival) return a;
    }
    return lat_conflict();
}

static inline int lat_equal(LatVal a, LatVal b) {
    if (a.state != b.state) return 0;
    if (a.state != LAT_CONST) return 1;
    if (a.isFloat != b.isFloat) return 0;
    return a.isFloat ? (a.val.fval == b.val.fval) : (a.val.ival == b.val.ival);
}

/* ---- ConstMap --------------------------------------------------------- */

typedef struct {
    LatVal *vals;
    int     size;
} ConstMap;

static void constMap_init(ConstMap *m, int size, Arena *arena) {
    m->size = size;
    m->vals = arena_alloc(arena, (size_t)size * sizeof(LatVal));
    for (int i = 0; i < size; i++) m->vals[i] = lat_unknown();
}

static inline void constMap_copy(ConstMap *dst, const ConstMap *src) {
    memcpy(dst->vals, src->vals, (size_t)src->size * sizeof(LatVal));
}

static inline int constMap_equal(const ConstMap *a, const ConstMap *b) {
    for (int i = 0; i < a->size; i++)
        if (!lat_equal(a->vals[i], b->vals[i])) return 0;
    return 1;
}

static void constMap_meet(ConstMap *dest, const ConstMap *src) {
    for (int i = 0; i < dest->size; i++)
        dest->vals[i] = lat_meet(dest->vals[i], src->vals[i]);
}

static inline LatVal constMap_get(const ConstMap *m, Operand op, VarMap *vm) {
    int id = operandVarId(vm, op);
    if (id < 0 || id >= m->size) return lat_conflict();
    return m->vals[id];
}

static inline Operand tryFold(Operand op, const ConstMap *m, VarMap *vm) {
    if (op.kind != OPND_VAR && op.kind != OPND_TEMP) return op;
    LatVal lv = constMap_get(m, op, vm);
    if (lv.state != LAT_CONST) return op;
    if (lv.isFloat) { Operand o; o.kind = OPND_CONST_FLOAT; o.data.floatVal = lv.val.fval; return o; }
    else            { Operand o; o.kind = OPND_CONST_INT;   o.data.intVal   = lv.val.ival; return o; }
}

/* ---- Helpers per transferInstr ---------------------------------------- */

static inline LatVal getLatVal(const ConstMap *map, Operand op, VarMap *vm) {
    switch (op.kind) {
    case OPND_CONST_INT:   return lat_const_int(op.data.intVal);
    case OPND_CONST_FLOAT: return lat_const_float(op.data.floatVal);
    case OPND_VAR:
    case OPND_TEMP:        return constMap_get(map, op, vm);
    default:               return lat_conflict();
    }
}

static inline int isBinaryOp(IROp op) {
    switch (op) {
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_LT:  case IR_LE:  case IR_GT:  case IR_GE:  case IR_EQ: case IR_NE:
        return 1;
    default: return 0;
    }
}

static inline int isComparisonOp(IROp op) {
    switch (op) {
    case IR_LT: case IR_LE: case IR_GT: case IR_GE: case IR_EQ: case IR_NE:
        return 1;
    default: return 0;
    }
}

static int foldBinaryInt(IROp op, int a, int b, int *res) {
    switch (op) {
    case IR_ADD: *res = a + b;                       return 1;
    case IR_SUB: *res = a - b;                       return 1;
    case IR_MUL: *res = a * b;                       return 1;
    case IR_DIV: if (!b) return 0; *res = a / b;     return 1;
    case IR_MOD: if (!b) return 0; *res = a % b;     return 1;
    case IR_LT:  *res = (a <  b);                    return 1;
    case IR_LE:  *res = (a <= b);                    return 1;
    case IR_GT:  *res = (a >  b);                    return 1;
    case IR_GE:  *res = (a >= b);                    return 1;
    case IR_EQ:  *res = (a == b);                    return 1;
    case IR_NE:  *res = (a != b);                    return 1;
    default:                                          return 0;
    }
}

static int foldBinaryFloat(IROp op, float a, float b, float *res) {
    switch (op) {
    case IR_ADD: *res = a + b;                            return 1;
    case IR_SUB: *res = a - b;                            return 1;
    case IR_MUL: *res = a * b;                            return 1;
    case IR_DIV: if (b == 0.0f) return 0; *res = a / b;  return 1;
    case IR_LT:  *res = (float)(a <  b);                  return 1;
    case IR_LE:  *res = (float)(a <= b);                  return 1;
    case IR_GT:  *res = (float)(a >  b);                  return 1;
    case IR_GE:  *res = (float)(a >= b);                  return 1;
    case IR_EQ:  *res = (float)(a == b);                  return 1;
    case IR_NE:  *res = (float)(a != b);                  return 1;
    default:                                               return 0;
    }
}

static LatVal foldUnary(IROp op, LatVal v) {
    if (v.state != LAT_CONST) return lat_conflict();
    if (!v.isFloat) {
        int i = v.val.ival;
        if (op == IR_NEG) return lat_const_int(-i);
        if (op == IR_NOT) return lat_const_int(!i);
    } else {
        float f = v.val.fval;
        if (op == IR_NEG) return lat_const_float(-f);
        if (op == IR_NOT) return lat_const_int(f == 0.0f ? 1 : 0);
    }
    return lat_conflict();
}

/* ---- Trasferimento: aggiorna la mappa simulando un'istruzione ---------- */
static void transferInstr(const IRInstr *in, ConstMap *map, VarMap *vm) {
    int id = operandVarId(vm, in->dst);
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

/* ---- Jump-to-next detection ------------------------------------------- */
static int is_jump_to_next(const IRFunction *f, int jumpIdx) {
    int targetLabel = f->instrs[jumpIdx].dst.data.labelId;
    for (int k = jumpIdx + 1; k < f->count; k++) {
        if (f->instrs[k].op == IR_LABEL) {
            if (f->instrs[k].dst.data.labelId == targetLabel)
                return 1;
            continue;
        }
        break;
    }
    return 0;
}

/* ---- Eliminazione label orfane ---------------------------------------- */
static int mark_unreferenced_labels(const IRFunction *f, char *eliminate) {
    int maxLabel = -1;
    for (int i = 0; i < f->count; i++) {
        if (eliminate[i]) continue;
        IROp op = f->instrs[i].op;
        if (op == IR_GOTO || op == IR_IF_FALSE) {
            int lbl = f->instrs[i].dst.data.labelId;
            if (lbl > maxLabel) maxLabel = lbl;
        }
    }

    if (maxLabel < 0) {
        int count = 0;
        for (int i = 0; i < f->count; i++) {
            if (!eliminate[i] && f->instrs[i].op == IR_LABEL) {
                eliminate[i] = 1;
                count++;
            }
        }
        return count;
    }

    int words = (maxLabel / 64) + 1;
    uint64_t *used = calloc((size_t)words, sizeof(uint64_t));
    if (!used) return 0;

    for (int i = 0; i < f->count; i++) {
        if (eliminate[i]) continue;
        IROp op = f->instrs[i].op;
        if (op == IR_GOTO || op == IR_IF_FALSE) {
            int lbl = f->instrs[i].dst.data.labelId;
            if (lbl >= 0 && lbl <= maxLabel)
                used[lbl >> 6] |= (1ULL << (lbl & 63));
        }
    }

    int count = 0;
    for (int i = 0; i < f->count; i++) {
        if (eliminate[i] || f->instrs[i].op != IR_LABEL) continue;
        int lbl = f->instrs[i].dst.data.labelId;
        int referenced = (lbl >= 0 && lbl <= maxLabel)
                         ? (int)((used[lbl >> 6] >> (lbl & 63)) & 1ULL)
                         : 0;
        if (!referenced) {
            eliminate[i] = 1;
            count++;
        }
    }

    free(used);
    return count;
}

/* ---- cp_optimize ------------------------------------------------------- */
int cp_optimize(IRFunction *f) {
    if (!f || f->blockCount == 0 || f->count == 0) return 0;

    int nBlocks = f->blockCount;
    Arena *arena = arena_create(0);

    /* PASSO 1: VarMap */
    VarMap vm;
    vm.table  = ht_create(32, cp_uint64_hash);
    vm.nextId = 0;
    for (int i = 0; i < f->count; i++) {
        IRInstr *in = &f->instrs[i];
        operandVarId(&vm, in->dst);
        operandVarId(&vm, in->src1);
        operandVarId(&vm, in->src2);
    }
    int numVars = vm.nextId;

    /* PASSO 2: In/Out */
    ConstMap *In  = arena_alloc(arena, (size_t)nBlocks * sizeof(ConstMap));
    ConstMap *Out = arena_alloc(arena, (size_t)nBlocks * sizeof(ConstMap));
    ConstMap  tmp; constMap_init(&tmp, numVars, arena);
    for (int b = 0; b < nBlocks; b++) {
        constMap_init(&In[b],  numVars, arena);
        constMap_init(&Out[b], numVars, arena);
    }

    /* PASSO 3: forward dataflow */
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int b = 0; b < nBlocks; b++) {
            int hasPred = 0;
            for (int p = 0; p < nBlocks; p++) {
                for (int k = 0; k < 2; k++) {
                    if (f->blocks[p].bb.succ[k] != b) continue;
                    if (!hasPred) { constMap_copy(&In[b], &Out[p]); hasPred = 1; }
                    else          constMap_meet(&In[b], &Out[p]);
                }
            }

            constMap_copy(&tmp, &In[b]);
            for (int i = f->blocks[b].bb.start; i < f->blocks[b].bb.end; i++)
                transferInstr(&f->instrs[i], &tmp, &vm);

            if (!constMap_equal(&Out[b], &tmp)) {
                constMap_copy(&Out[b], &tmp);
                changed = 1;
            }
        }
    }

    /* ---- PASSO 4: riscrittura + CFG pruning + folding binario ---- */
    int modified = 0;

    char *eliminate = arena_alloc(arena, (size_t)f->count * sizeof(char));
    memset(eliminate, 0, (size_t)f->count * sizeof(char));

    for (int b = 0; b < nBlocks; b++) {
        ConstMap live; constMap_init(&live, numVars, arena);
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
                continue;
            }

            if (in->op == IR_IF_FALSE) {
                Operand cond = tryFold(in->src1, &live, &vm);
                if (cond.kind == OPND_CONST_INT || cond.kind == OPND_CONST_FLOAT) {
                    int isZero = (cond.kind == OPND_CONST_INT)
                                 ? (cond.data.intVal == 0)
                                 : (cond.data.floatVal == 0.0f);
                    int taken  = isZero;
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
                    in->src1 = cond; modified = 1;
                }
                transferInstr(in, &live, &vm);
                continue;
            }

            Operand ns1 = tryFold(in->src1, &live, &vm);
            Operand ns2 = tryFold(in->src2, &live, &vm);
            if (!operand_equal(&ns1, &in->src1)) { in->src1 = ns1; modified = 1; }
            if (!operand_equal(&ns2, &in->src2)) { in->src2 = ns2; modified = 1; }

            /* ---- FOLDING BINARIO ---- */
            if (in->op != IR_IF_FALSE && in->op != IR_LABEL &&
                in->op != IR_GOTO     && in->op != IR_RETURN &&
                (in->op == IR_ADD || in->op == IR_SUB || in->op == IR_MUL ||
                 in->op == IR_DIV || in->op == IR_MOD ||
                 in->op == IR_LT  || in->op == IR_LE  || in->op == IR_GT  ||
                 in->op == IR_GE  || in->op == IR_EQ  || in->op == IR_NE)) {

                if ((ns1.kind == OPND_CONST_INT || ns1.kind == OPND_CONST_FLOAT) &&
                    (ns2.kind == OPND_CONST_INT || ns2.kind == OPND_CONST_FLOAT)) {

                    int floatOp = (ns1.kind == OPND_CONST_FLOAT ||
                                   ns2.kind == OPND_CONST_FLOAT);
                    int ok = 1;
                    int resultInt = 0;
                    float resultFloat = 0.0f;

                    if (floatOp) {
                        float a = (ns1.kind == OPND_CONST_INT)
                                  ? (float)ns1.data.intVal : ns1.data.floatVal;
                        float b = (ns2.kind == OPND_CONST_INT)
                                  ? (float)ns2.data.intVal : ns2.data.floatVal;
                        switch (in->op) {
                            case IR_ADD: resultFloat = a + b; break;
                            case IR_SUB: resultFloat = a - b; break;
                            case IR_MUL: resultFloat = a * b; break;
                            case IR_DIV:
                                if (b == 0.0f) ok = 0;
                                else resultFloat = a / b;
                                break;
                            case IR_LT:  resultFloat = (float)(a <  b); break;
                            case IR_LE:  resultFloat = (float)(a <= b); break;
                            case IR_GT:  resultFloat = (float)(a >  b); break;
                            case IR_GE:  resultFloat = (float)(a >= b); break;
                            case IR_EQ:  resultFloat = (float)(a == b); break;
                            case IR_NE:  resultFloat = (float)(a != b); break;
                            default: ok = 0; break;
                        }
                    } else {
                        int a = ns1.data.intVal;
                        int b = ns2.data.intVal;
                        switch (in->op) {
                            case IR_ADD: resultInt = a + b; break;
                            case IR_SUB: resultInt = a - b; break;
                            case IR_MUL: resultInt = a * b; break;
                            case IR_DIV:
                                if (b == 0) ok = 0;
                                else resultInt = a / b;
                                break;
                            case IR_MOD:
                                if (b == 0) ok = 0;
                                else resultInt = a % b;
                                break;
                            case IR_LT:  resultInt = (a <  b); break;
                            case IR_LE:  resultInt = (a <= b); break;
                            case IR_GT:  resultInt = (a >  b); break;
                            case IR_GE:  resultInt = (a >= b); break;
                            case IR_EQ:  resultInt = (a == b); break;
                            case IR_NE:  resultInt = (a != b); break;
                            default: ok = 0; break;
                        }
                    }

                    if (ok) {
                        IROp origOp = in->op;
                        in->op = IR_ASSIGN;
                        if (floatOp && (origOp == IR_LT || origOp == IR_LE ||
                                        origOp == IR_GT || origOp == IR_GE ||
                                        origOp == IR_EQ || origOp == IR_NE)) {
                            in->src1.kind = OPND_CONST_INT;
                            in->src1.data.intVal = (int)resultFloat;
                        } else if (floatOp) {
                            in->src1.kind = OPND_CONST_FLOAT;
                            in->src1.data.floatVal = resultFloat;
                        } else {
                            in->src1.kind = OPND_CONST_INT;
                            in->src1.data.intVal = resultInt;
                        }
                        in->src2 = noOperand();
                        modified = 1;
                    }
                }
            }

            transferInstr(in, &live, &vm);
        }
    }

    /* ---- Rimozione label orfane ---- */
    if (mark_unreferenced_labels(f, eliminate) > 0) modified = 1;

    /* ---- PASSO 5: Sweep — aggiorna blocchi inline, niente map[] ---- */
    if (modified) {
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
    }

    arena_destroy(arena);
    ht_destroy(vm.table, NULL);
    return modified;
}