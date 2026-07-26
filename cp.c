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

static inline int lat_equal(LatVal a, LatVal b) {
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

static inline void constMap_init(ConstMap *m, int size, Arena *arena) {
    m->size = size;
    m->vals = arena_alloc(arena, (size_t)size * sizeof(LatVal));
    for (int i = 0; i < size; i++) m->vals[i] = lat_unknown();
}

static inline void constMap_copy(ConstMap *dst, const ConstMap *src) {
    memcpy(dst->vals, src->vals, (size_t)src->size * sizeof(LatVal));
}

static int constMap_equal(const ConstMap *a, const ConstMap *b) {
    for (int i = 0; i < a->size; i++)
        if (!lat_equal(a->vals[i], b->vals[i])) return 0;
    return 1;
}

/* Meet in-place: dest = meet(dest, src) per ogni variabile. */
static inline void constMap_meet(ConstMap *dest, const ConstMap *src) {
    for (int i = 0; i < dest->size; i++)
        dest->vals[i] = lat_meet(dest->vals[i], src->vals[i]);
}

/* Legge il valore della variabile/temp indicata da op nella mappa. */
static inline LatVal constMap_get(const ConstMap *m, Operand op, VarMap *vm) {
    int id = operandVarId(vm, op);
    if (id < 0 || id >= m->size) return lat_conflict();
    return m->vals[id];
}

/* Trasforma un operando in costante se il suo valore e' noto. */
static inline Operand tryFold(Operand op, const ConstMap *m, VarMap *vm) {
    if (op.kind != OPND_VAR && op.kind != OPND_TEMP) return op;
    LatVal lv = constMap_get(m, op, vm);
    if (lv.state != LAT_CONST) return op;
    if (lv.isFloat) { Operand o; o.kind = OPND_CONST_FLOAT; o.data.floatVal = lv.val.fval; return o; }
    else            { Operand o; o.kind = OPND_CONST_INT;   o.data.intVal   = lv.val.ival; return o; }
}

/* ---- Trasferimento: aggiorna Out simulando un'istruzione -------------- */
// static void transferInstr(const IRInstr *in, ConstMap *map, VarMap *vm) {
//     /* Solo IR_ASSIGN con sorgente costante o variabile nota propaga. */
//     if (in->op != IR_ASSIGN) {
//         /* Qualunque altra istruzione che scrive dst: azzera la costante. */
//         int id = operandVarId(vm, in->dst);
//         if (id >= 0 && id < map->size) map->vals[id] = lat_conflict();
//         return;
//     }
//     int id = operandVarId(vm, in->dst);
//     if (id < 0 || id >= map->size) return;

//     Operand src = in->src1;
//     if (src.kind == OPND_CONST_INT)   { map->vals[id] = lat_const_int(src.data.intVal);   return; }
//     if (src.kind == OPND_CONST_FLOAT) { map->vals[id] = lat_const_float(src.data.floatVal); return; }
//     if (src.kind == OPND_VAR || src.kind == OPND_TEMP) {
//         LatVal lv = constMap_get(map, src, vm);
//         map->vals[id] = lv;   /* propaga il valore del sorgente (anche UNKNOWN/CONFLICT) */
//         return;
//     }
//     /* sorgente non costante e non variabile tracciata */
//     map->vals[id] = lat_conflict();
// }
/* ---- Trasferimento: aggiorna Out simulando un'istruzione -------------- */
static void transferInstr(const IRInstr *in, ConstMap *map, VarMap *vm) {
    int id = operandVarId(vm, in->dst);
    if (id < 0 || id >= map->size) return;

    /* IR_ASSIGN: propaga il valore del sorgente */
    if (in->op == IR_ASSIGN) {
        Operand src = in->src1;
        if (src.kind == OPND_CONST_INT) {
            map->vals[id] = lat_const_int(src.data.intVal);
            return;
        }
        if (src.kind == OPND_CONST_FLOAT) {
            map->vals[id] = lat_const_float(src.data.floatVal);
            return;
        }
        if (src.kind == OPND_VAR || src.kind == OPND_TEMP) {
            LatVal lv = constMap_get(map, src, vm);
            map->vals[id] = lv;
            return;
        }
        map->vals[id] = lat_conflict();
        return;
    }

    /* ---- Operatori binari (aritmetici e di confronto) ---- */
    if (in->op == IR_ADD || in->op == IR_SUB || in->op == IR_MUL ||
        in->op == IR_DIV || in->op == IR_MOD ||
        in->op == IR_LT  || in->op == IR_LE  || in->op == IR_GT  ||
        in->op == IR_GE  || in->op == IR_EQ  || in->op == IR_NE) {

        /* Ottieni il valore di src1 (costante letterale o da mappa) */
        LatVal lv1;
        if (in->src1.kind == OPND_CONST_INT)
            lv1 = lat_const_int(in->src1.data.intVal);
        else if (in->src1.kind == OPND_CONST_FLOAT)
            lv1 = lat_const_float(in->src1.data.floatVal);
        else
            lv1 = constMap_get(map, in->src1, vm);

        /* Ottieni il valore di src2 */
        LatVal lv2;
        if (in->src2.kind == OPND_CONST_INT)
            lv2 = lat_const_int(in->src2.data.intVal);
        else if (in->src2.kind == OPND_CONST_FLOAT)
            lv2 = lat_const_float(in->src2.data.floatVal);
        else
            lv2 = constMap_get(map, in->src2, vm);

        /* Se entrambi sono costanti e dello stesso tipo, calcola il risultato */
        if (lv1.state == LAT_CONST && lv2.state == LAT_CONST) {
            /* Caso intero */
            if (!lv1.isFloat && !lv2.isFloat) {
                int a = lv1.val.ival, b = lv2.val.ival;
                int result = 0;
                int ok = 1;
                switch (in->op) {
                    case IR_ADD: result = a + b; break;
                    case IR_SUB: result = a - b; break;
                    case IR_MUL: result = a * b; break;
                    case IR_DIV: if (b == 0) ok = 0; else result = a / b; break;
                    case IR_MOD: if (b == 0) ok = 0; else result = a % b; break;
                    case IR_LT:  result = a < b; break;
                    case IR_LE:  result = a <= b; break;
                    case IR_GT:  result = a > b; break;
                    case IR_GE:  result = a >= b; break;
                    case IR_EQ:  result = a == b; break;
                    case IR_NE:  result = a != b; break;
                    default: ok = 0; break;
                }
                if (ok) {
                    map->vals[id] = lat_const_int(result);
                    return;
                }
            }
            /* Caso float */
            else if (lv1.isFloat && lv2.isFloat) {
                float a = lv1.val.fval, b = lv2.val.fval;
                float result = 0.0f;
                int ok = 1;
                switch (in->op) {
                    case IR_ADD: result = a + b; break;
                    case IR_SUB: result = a - b; break;
                    case IR_MUL: result = a * b; break;
                    case IR_DIV: if (b == 0.0f) ok = 0; else result = a / b; break;
                    case IR_LT:  result = (float)(a < b); break;
                    case IR_LE:  result = (float)(a <= b); break;
                    case IR_GT:  result = (float)(a > b); break;
                    case IR_GE:  result = (float)(a >= b); break;
                    case IR_EQ:  result = (float)(a == b); break;
                    case IR_NE:  result = (float)(a != b); break;
                    default: ok = 0; break;
                }
                if (ok) {
                    /* I confronti restituiscono un intero, non float */
                    if (in->op == IR_LT || in->op == IR_LE || in->op == IR_GT ||
                        in->op == IR_GE || in->op == IR_EQ || in->op == IR_NE) {
                        map->vals[id] = lat_const_int((int)result);
                    } else {
                        map->vals[id] = lat_const_float(result);
                    }
                    return;
                }
            }
        }

        /* Se non possiamo piegare, marca come conflict (non costante) */
        map->vals[id] = lat_conflict();
        return;
    }

    /* ---- Operatori unari (NEG, NOT) ---- */
    if (in->op == IR_NEG || in->op == IR_NOT) {
        LatVal lv;
        if (in->src1.kind == OPND_CONST_INT)
            lv = lat_const_int(in->src1.data.intVal);
        else if (in->src1.kind == OPND_CONST_FLOAT)
            lv = lat_const_float(in->src1.data.floatVal);
        else
            lv = constMap_get(map, in->src1, vm);

        if (lv.state == LAT_CONST) {
            if (!lv.isFloat) {
                int v = lv.val.ival;
                if (in->op == IR_NEG) map->vals[id] = lat_const_int(-v);
                else                 map->vals[id] = lat_const_int(!v);
            } else {
                float v = lv.val.fval;
                if (in->op == IR_NEG) map->vals[id] = lat_const_float(-v);
                else                 map->vals[id] = lat_const_int(v == 0.0f ? 1 : 0);
            }
            return;
        }
        map->vals[id] = lat_conflict();
        return;
    }

    /* ---- Qualunque altra istruzione che definisce dst (LOAD, CALL, ...) ---- */
    map->vals[id] = lat_conflict();
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
