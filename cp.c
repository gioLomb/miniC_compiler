#include <stdlib.h>
#include <string.h>
#include "cp.h"
#include "arena.h"

/* ---- Reticolo dei valori -----------------------------------------------
 *
 * Ogni variabile/temporaneo in un dato punto del programma si trova in
 * uno di tre stati nel reticolo:
 *
 *   TOP (UNKNOWN): non abbiamo ancora visto nessuna definizione che
 *       raggiunga questo punto — valore non ancora determinato.
 *       E' lo stato iniziale per tutti i blocchi tranne l'entry.
 *
 *   CONST: tutte le definizioni che raggiungono questo punto assegnano
 *       lo stesso valore costante. Propagiamo.
 *
 *   BOTTOM (CONFLICT): due o piu' definizioni con valori diversi
 *       raggiungono questo punto — non possiamo propagare nulla.
 *
 * Il meet (∩) di due stati:
 *   TOP    ∩ X      = X         (TOP e' l'elemento neutro)
 *   CONST(k) ∩ CONST(k) = CONST(k)
 *   CONST(k) ∩ CONST(j) = BOTTOM   (k != j)
 *   BOTTOM ∩ X      = BOTTOM    (BOTTOM assorbe tutto)
 *
 * I valori scendono TOP → CONST → BOTTOM e non risalgono mai:
 * garantisce la terminazione del punto fisso. */

#define LAT_UNKNOWN  0   /* TOP:    nessuna definizione ancora vista */
#define LAT_CONST    1   /* CONST:  valore costante noto */
#define LAT_CONFLICT 2   /* BOTTOM: valori diversi, non propagabile */

/* Lattice value per una singola variabile/temporaneo. */
typedef struct {
    int state;       /* LAT_UNKNOWN / LAT_CONST / LAT_CONFLICT */
    int isFloat;     /* 0 = intero, 1 = float */
    union {
        int   ival;
        float fval;
    } val;
} LatVal;

/* ---- Mappa variabile -> ID compatto ------------------------------------
 * Identica alla VarMap in dce.c: mappa (kind, level/tempId, offset) a
 * un indice intero compatto, usato come indice negli array LatVal[]. */

#include <stdint.h>
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

static uint64_t cp_make_key(int kind, int a, int b) {
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

static int operandVarId(VarMap *m, Operand op) {
    if (op.kind == OPND_VAR)  return varMap_id(m, 0, op.data.varLevel, op.data.varOffset);
    if (op.kind == OPND_TEMP) return varMap_id(m, 1, op.data.tempId, 0);
    return -1;
}

/* ---- Operazioni sul reticolo ------------------------------------------ */

static LatVal lat_unknown(void) {
    LatVal v = {0}; v.state = LAT_UNKNOWN; return v;
}

static LatVal lat_const_int(int ival) {
    LatVal v = {0}; v.state = LAT_CONST; v.isFloat = 0; v.val.ival = ival; return v;
}

static LatVal lat_const_float(float fval) {
    LatVal v = {0}; v.state = LAT_CONST; v.isFloat = 1; v.val.fval = fval; return v;
}

static LatVal lat_conflict(void) {
    LatVal v = {0}; v.state = LAT_CONFLICT; return v;
}

/* Meet: prende il minimo (piu' conservativo) dei due stati. */
static LatVal lat_meet(LatVal a, LatVal b) {
    if (a.state == LAT_UNKNOWN) return b;
    if (b.state == LAT_UNKNOWN) return a;
    if (a.state == LAT_CONFLICT || b.state == LAT_CONFLICT) return lat_conflict();
    /* entrambi CONST */
    if (a.isFloat != b.isFloat) return lat_conflict();
    if (a.isFloat) {
        if (a.val.fval == b.val.fval) return a;
    } else {
        if (a.val.ival == b.val.ival) return a;
    }
    return lat_conflict();
}

static int lat_equal(LatVal a, LatVal b) {
    if (a.state != b.state) return 0;
    if (a.state != LAT_CONST) return 1;
    if (a.isFloat != b.isFloat) return 0;
    return a.isFloat ? (a.val.fval == b.val.fval) : (a.val.ival == b.val.ival);
}

/* ---- ConstMap: array di LatVal indicizzato per variable ID ------------ */

typedef struct {
    LatVal *vals;   /* vals[varId] */
    int     size;   /* numero di variabili tracciate */
} ConstMap;

static void constMap_init(ConstMap *m, int size, Arena *arena) {
    m->size = size;
    m->vals = arena_alloc(arena, (size_t)size * sizeof(LatVal));
    for (int i = 0; i < size; i++) m->vals[i] = lat_unknown();
}

static void constMap_copy(ConstMap *dst, const ConstMap *src) {
    memcpy(dst->vals, src->vals, (size_t)src->size * sizeof(LatVal));
}

static int constMap_equal(const ConstMap *a, const ConstMap *b) {
    for (int i = 0; i < a->size; i++)
        if (!lat_equal(a->vals[i], b->vals[i])) return 0;
    return 1;
}

/* Meet in-place: dest = meet(dest, src) per ogni variabile. */
static void constMap_meet(ConstMap *dest, const ConstMap *src) {
    for (int i = 0; i < dest->size; i++)
        dest->vals[i] = lat_meet(dest->vals[i], src->vals[i]);
}

/* Legge il valore della variabile/temp indicata da op nella mappa. */
static LatVal constMap_get(const ConstMap *m, Operand op, VarMap *vm) {
    int id = operandVarId(vm, op);
    if (id < 0 || id >= m->size) return lat_conflict();
    return m->vals[id];
}

/* Trasforma un operando in costante se il suo valore e' noto. */
static Operand tryFold(Operand op, const ConstMap *m, VarMap *vm) {
    if (op.kind != OPND_VAR && op.kind != OPND_TEMP) return op;
    LatVal lv = constMap_get(m, op, vm);
    if (lv.state != LAT_CONST) return op;
    if (lv.isFloat) { Operand o; o.kind = OPND_CONST_FLOAT; o.data.floatVal = lv.val.fval; return o; }
    else            { Operand o; o.kind = OPND_CONST_INT;   o.data.intVal   = lv.val.ival; return o; }
}

/* ---- Helpers per transferInstr ---------------------------------------- */

/* Restituisce il LatVal di un operando: costante diretta, oppure lookup
 * nella mappa per variabili/temporanei, oppure CONFLICT per tutto il resto. */
static LatVal getLatVal(const ConstMap *map, Operand op, VarMap *vm) {
    switch (op.kind) {
    case OPND_CONST_INT:   return lat_const_int(op.data.intVal);
    case OPND_CONST_FLOAT: return lat_const_float(op.data.floatVal);
    case OPND_VAR:
    case OPND_TEMP:        return constMap_get(map, op, vm);
    default:               return lat_conflict();
    }
}

/* 1 se l'op e' binario puro (aritmetica o confronto), 0 altrimenti. */
static int isBinaryOp(IROp op) {
    switch (op) {
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_LT:  case IR_LE:  case IR_GT:  case IR_GE:  case IR_EQ: case IR_NE:
        return 1;
    default: return 0;
    }
}

/* 1 se l'op e' un confronto (restituisce 0/1 intero). */
static int isComparisonOp(IROp op) {
    switch (op) {
    case IR_LT: case IR_LE: case IR_GT: case IR_GE: case IR_EQ: case IR_NE:
        return 1;
    default: return 0;
    }
}

/* Constant folding intero. Ritorna 1 se ha successo, 0 su div/mod per zero. */
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

/* Constant folding float. Confronti restituiscono 0.0f/1.0f. */
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

/* Constant folding unario. */
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

/* ---- Trasferimento: aggiorna la mappa simulando un'istruzione ----------
 * Dispatcher pulito: estrazione dei valori delegata a getLatVal, calcolo
 * delegato agli helper di folding. Unica scrittura nella mappa alla fine. */
static void transferInstr(const IRInstr *in, ConstMap *map, VarMap *vm) {
    int id = operandVarId(vm, in->dst);
    if (id < 0 || id >= map->size) return;

    LatVal result = lat_conflict();   /* fallback sicuro */

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

/* ---- cp_optimize ------------------------------------------------------- */
int cp_optimize(IRFunction *f) {
    if (!f || f->blockCount == 0 || f->count == 0) return 0;

    int nBlocks = f->blockCount;
    Arena *arena = arena_create(0);

    /* ---- PASSO 1: costruisci VarMap scansionando tutte le istruzioni ---- */
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

    /* ---- PASSO 2: alloca In/Out per ogni blocco ------------------------- */
    ConstMap *In  = arena_alloc(arena, (size_t)nBlocks * sizeof(ConstMap));
    ConstMap *Out = arena_alloc(arena, (size_t)nBlocks * sizeof(ConstMap));
    ConstMap  tmp; constMap_init(&tmp, numVars, arena);

    for (int b = 0; b < nBlocks; b++) {
        constMap_init(&In[b],  numVars, arena);
        constMap_init(&Out[b], numVars, arena);
    }

    /* ---- PASSO 3: forward dataflow a punto fisso ------------------------
     * In[entry] = tutto UNKNOWN (non ci sono predecessori).
     * In[b]     = meet di tutti Out[pred] per b != entry.
     * Out[b]    = transfer(In[b], istruzioni di b).
     * Itera finche' nessun Out cambia. */
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int b = 0; b < nBlocks; b++) {
            /* Calcola In[b] = meet dei Out dei predecessori */
            int hasPred = 0;
            for (int p = 0; p < nBlocks; p++) {
                for (int k = 0; k < 2; k++) {
                    if (f->blocks[p].succ[k] != b) continue;
                    if (!hasPred) { constMap_copy(&In[b], &Out[p]); hasPred = 1; }
                    else          constMap_meet(&In[b], &Out[p]);
                }
            }
            /* b=0 (entry) ha hasPred=0: In[0] resta UNKNOWN — corretto */

            /* Calcola Out[b] = transfer(In[b]) */
            constMap_copy(&tmp, &In[b]);
            for (int i = f->blocks[b].start; i < f->blocks[b].end; i++)
                transferInstr(&f->instrs[i], &tmp, &vm);

            if (!constMap_equal(&Out[b], &tmp)) {
                constMap_copy(&Out[b], &tmp);
                changed = 1;
            }
        }
    }

    /* ---- PASSO 4: riscrittura + CFG pruning ----------------------------
     * Per ogni istruzione, sostituiamo gli operandi con la costante nota
     * in In[blocco] aggiornata progressivamente istruzione per istruzione.
     * Se un IR_IF_FALSE legge una condizione costante, lo trasformiamo:
     *   - costante != 0: il salto NON viene preso → rimuovi IR_IF_FALSE,
     *     il fallthrough diventa l'unico successore.
     *   - costante == 0: il salto VIENE preso → trasforma in IR_GOTO,
     *     succ[1] diventa l'unico successore.
     * In entrambi i casi aggiorniamo succ[]/predCount del CFG. */
    int modified = 0;

    /* instrToBlock per aggiornare start/end dopo riscrittura */
    int *instrToBlock = arena_alloc(arena, (size_t)f->count * sizeof(int));
    for (int b = 0; b < nBlocks; b++)
        for (int i = f->blocks[b].start; i < f->blocks[b].end; i++)
            instrToBlock[i] = b;

    /* Mappa istruzione -> "da eliminare" (IR_IF_FALSE con branch statico) */
    char *eliminate = arena_alloc(arena, (size_t)f->count * sizeof(char));
    memset(eliminate, 0, (size_t)f->count * sizeof(char));

    for (int b = 0; b < nBlocks; b++) {
        /* Ricostruisci lo stato live istruzione per istruzione */
        ConstMap live; constMap_init(&live, numVars, arena);
        constMap_copy(&live, &In[b]);

        for (int i = f->blocks[b].start; i < f->blocks[b].end; i++) {
            IRInstr *in = &f->instrs[i];

            if (in->op == IR_IF_FALSE) {
                /* Prova a piegare la condizione */
                Operand cond = tryFold(in->src1, &live, &vm);
                if (cond.kind == OPND_CONST_INT || cond.kind == OPND_CONST_FLOAT) {
                    int isZero = (cond.kind == OPND_CONST_INT)
                                 ? (cond.data.intVal == 0)
                                 : (cond.data.floatVal == 0.0f);
                    int taken  = isZero;   /* IF_FALSE: salto se condizione == 0 */

                    int s0 = f->blocks[b].succ[0];   /* fallthrough */
                    int s1 = f->blocks[b].succ[1];   /* bersaglio del salto */

                    if (taken) {
                        /* Salto sempre preso: IF_FALSE → GOTO bersaglio */
                        in->op   = IR_GOTO;
                        in->src1 = in->src2 = (Operand){.kind = OPND_NONE};
                        /* dst rimane il labelId del bersaglio */
                        f->blocks[b].succ[0] = s1;
                        f->blocks[b].succ[1] = -1;
                        if (s0 >= 0) f->blocks[s0].predCount--;
                    } else {
                        /* Salto mai preso: elimina IF_FALSE, fallthrough resta */
                        eliminate[i] = 1;
                        f->blocks[b].succ[1] = -1;
                        if (s1 >= 0) f->blocks[s1].predCount--;
                    }
                    modified = 1;
                } else if (cond.kind != in->src1.kind ||
                           (cond.kind == OPND_VAR  && (cond.data.varLevel != in->src1.data.varLevel ||
                                                        cond.data.varOffset != in->src1.data.varOffset)) ||
                           (cond.kind == OPND_TEMP && cond.data.tempId != in->src1.data.tempId)) {
                    in->src1 = cond; modified = 1;
                }
                transferInstr(in, &live, &vm);
                continue;
            }

            /* Sostituisci src1 e src2 con costanti note */
            Operand ns1 = tryFold(in->src1, &live, &vm);
            Operand ns2 = tryFold(in->src2, &live, &vm);
            if (ns1.kind != in->src1.kind || memcmp(&ns1, &in->src1, sizeof(Operand))) {
                in->src1 = ns1; modified = 1;
            }
            if (ns2.kind != in->src2.kind || memcmp(&ns2, &in->src2, sizeof(Operand))) {
                in->src2 = ns2; modified = 1;
            }

            transferInstr(in, &live, &vm);
        }
    }

    /* ---- PASSO 5: Sweep delle istruzioni eliminate -------------------- */
    if (modified) {
        int *map = arena_alloc(arena, (size_t)f->count * sizeof(int));
        for (int i = 0; i < f->count; i++) map[i] = -1;

        IRInstr *newInstrs = malloc((size_t)f->count * sizeof(IRInstr));
        int newCount = 0;
        for (int i = 0; i < f->count; i++) {
            if (!eliminate[i]) { newInstrs[newCount] = f->instrs[i]; map[i] = newCount++; }
        }
        free(f->instrs);
        f->instrs  = newInstrs;
        f->count   = newCount;
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
    }

    arena_destroy(arena);
    ht_destroy(vm.table, NULL);
    return modified;
}