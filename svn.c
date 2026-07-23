#include <stdlib.h>
#include <string.h>
#include "svn.h"
#include "hash_table.h"

/* ---- Strutture per la sheaf di tabelle ---- */
#define SVN_MAX_NAMES 4
#define SVN_SCOPE_TABLE_CAPACITY 7

typedef struct SVNScope {
    Hash_Table *values;    /* ValueKey -> vn (int) */
    Hash_Table *exprs;     /* ExprKey  -> vn (int) */
    Hash_Table *leaders;   /* vn (int) -> NameList */
    struct SVNScope *parent;
} SVNScope;

typedef struct {
    int kind;                 /* 0=var, 1=temp, 2=constInt, 3=constFloat, -1=non pertinente */
    union {
        struct {
            int varLevel;
            int varOffset;
        };
        int tempId;
        long intVal;
        double floatVal;
    } data;
} ValueKey;

typedef struct {
    int op;
    int vn1, vn2;       /* vn2 = -1 per operatori unari */
} ExprKey;

typedef struct {
    int count;
    Operand names[SVN_MAX_NAMES];
} NameList;

static unsigned long svnHash(const void *key, size_t keySize) {
    const unsigned char *bytes = key;
    unsigned long h = 2166136261UL;
    for (size_t i = 0; i < keySize; i++) { h ^= bytes[i]; h *= 16777619UL; }
    return h;
}

static void svnScopeInit(SVNScope *scope, SVNScope *parent) {
    scope->values  = ht_create(SVN_SCOPE_TABLE_CAPACITY, svnHash);
    scope->exprs   = ht_create(SVN_SCOPE_TABLE_CAPACITY, svnHash);
    scope->leaders = ht_create(SVN_SCOPE_TABLE_CAPACITY, svnHash);
    scope->parent  = parent;
}

static void svnScopeDestroy(SVNScope *scope) {
    ht_destroy(scope->values, NULL);
    ht_destroy(scope->exprs, NULL);
    ht_destroy(scope->leaders, NULL);
}

static Operand mkNone(void) {
    Operand o; o.kind = OPND_NONE; return o;
}

/* La chiave viene passata per puntatore invece di essere ritornata per
 * valore: un return-by-value di una struct con padding interno puo'
 * lasciare i byte di padding non azzerati nella copia che il compilatore
 * scrive nel frame del chiamante, anche se il memset li aveva azzerati
 * prima dell'assegnazione dei campi (il compilatore e' libero di copiare
 * solo i campi definiti, non il padding, ottimizzando la "copia" della
 * struct). Passare l'indirizzo della variabile locale del chiamante e
 * scrivere direttamente li' evita qualunque copia intermedia: il memset
 * e le assegnazioni successivi avvengono sugli stessi byte che la
 * hash table leggera come chiave, senza intermediari. */
static void keyForOperand(const Operand *op, ValueKey *k) {
    memset(k, 0, sizeof(*k));
    switch (op->kind) {
    case OPND_VAR:
        k->kind = 0;
        k->data.varLevel = op->data.varLevel;
        k->data.varOffset = op->data.varOffset;
        break;
    case OPND_TEMP:
        k->kind = 1;
        k->data.tempId = op->data.tempId;
        break;
    case OPND_CONST_INT:
        k->kind = 2;
        k->data.intVal = op->data.intVal;
        break;
    case OPND_CONST_FLOAT:
        k->kind = 3;
        k->data.floatVal = op->data.floatVal;
        break;
    default:
        k->kind = -1;
        break;
    }
}

static void addNameForValue(int vn, Operand name, SVNScope *scope) {
    NameList list;
    memset(&list, 0, sizeof list);
    for (SVNScope *s = scope; s; s = s->parent) {
        if (ht_get(s->leaders, &vn, sizeof(vn), &list, sizeof(list))) break;
    }
    if (list.count < SVN_MAX_NAMES) {
        list.names[list.count++] = name;
    }
    ht_set(scope->leaders, &vn, sizeof(vn), &list, sizeof(list));
}

static void defineValue(Operand dst, int vn, SVNScope *scope) {
    if (dst.kind != OPND_VAR && dst.kind != OPND_TEMP) return;
    ValueKey k; keyForOperand(&dst, &k);
    ht_set(scope->values, &k, sizeof(k), &vn, sizeof(vn));
    addNameForValue(vn, dst, scope);
}

static int nameStillValid(Operand name, int vn, SVNScope *scope) {
    if (name.kind != OPND_VAR) return 1;
    ValueKey k; keyForOperand(&name, &k);
    for (SVNScope *s = scope; s; s = s->parent) {
        int currentVN;
        if (ht_get(s->values, &k, sizeof(k), &currentVN, sizeof(currentVN)))
            return currentVN == vn;
    }
    return 0;
}

static int findValidLeader(int vn, SVNScope *scope, Operand *outLeader) {
    NameList list;
    memset(&list, 0, sizeof list);
    int haveList = 0;
    for (SVNScope *s = scope; s; s = s->parent) {
        if (ht_get(s->leaders, &vn, sizeof(vn), &list, sizeof(list))) { haveList = 1; break; }
    }
    if (!haveList) return 0;
    for (int i = 0; i < list.count; i++) {
        if (nameStillValid(list.names[i], vn, scope)) {
            *outLeader = list.names[i];
            return 1;
        }
    }
    return 0;
}

static int valueNumberOf(Operand op, SVNScope *scope, int *nextVN) {
    ValueKey k; keyForOperand(&op, &k);
    if (k.kind < 0) return -1;
    int vn;
    for (SVNScope *s = scope; s; s = s->parent) {
        if (ht_get(s->values, &k, sizeof(k), &vn, sizeof(vn))) return vn;
    }
    vn = (*nextVN)++;
    ht_set(scope->values, &k, sizeof(k), &vn, sizeof(vn));
    addNameForValue(vn, op, scope);
    return vn;
}

static int isCommutative(IROp op) {
    return op == IR_ADD || op == IR_MUL || op == IR_EQ || op == IR_NE;
}

static void memoizeOrRewrite(IRInstr *in, ExprKey ek, SVNScope *scope, int *nextVN) {
    int exprVN;
    int found = 0;
    for (SVNScope *s = scope; s; s = s->parent) {
        if (ht_get(s->exprs, &ek, sizeof(ek), &exprVN, sizeof(exprVN))) { found = 1; break; }
    }
    Operand leader;
    if (found && findValidLeader(exprVN, scope, &leader)) {
        in->op = IR_ASSIGN;
        in->src1 = leader;
        in->src2 = mkNone();
        defineValue(in->dst, exprVN, scope);
        return;
    }
    exprVN = (*nextVN)++;
    ht_set(scope->exprs, &ek, sizeof(ek), &exprVN, sizeof(exprVN));
    defineValue(in->dst, exprVN, scope);
}

static void svnProcessInstr(IRInstr *in, SVNScope *scope, int *nextVN) {
    switch (in->op) {
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_LT:  case IR_LE:  case IR_GT:  case IR_GE:  case IR_EQ: case IR_NE: {
        int vn1 = valueNumberOf(in->src1, scope, nextVN);
        int vn2 = valueNumberOf(in->src2, scope, nextVN);
        if (isCommutative(in->op) && vn1 > vn2) { int t = vn1; vn1 = vn2; vn2 = t; }
        ExprKey ek;
        memset(&ek, 0, sizeof ek);
        ek.op = (int) in->op; ek.vn1 = vn1; ek.vn2 = vn2;
        memoizeOrRewrite(in, ek, scope, nextVN);
        break;
    }
    case IR_NEG: case IR_NOT: {
        int vn1 = valueNumberOf(in->src1, scope, nextVN);
        ExprKey ek;
        memset(&ek, 0, sizeof ek);
        ek.op = (int) in->op; ek.vn1 = vn1; ek.vn2 = -1;
        memoizeOrRewrite(in, ek, scope, nextVN);
        break;
    }
    case IR_ASSIGN: {
        int vn1 = valueNumberOf(in->src1, scope, nextVN);
        defineValue(in->dst, vn1, scope);
        break;
    }
    case IR_LOAD_ARR:
    case IR_CALL: {
        int fresh = (*nextVN)++;
        defineValue(in->dst, fresh, scope);
        break;
    }
    case IR_STORE_ARR:
    case IR_PARAM:
    case IR_RETURN:
    case IR_GOTO:
    case IR_IF_FALSE:
    case IR_LABEL:
        break;
    }
}

static void svnProcessBlock(IRFunction *f, int blockIdx, SVNScope *parent, int *nextVN) {
    SVNScope scope;
    svnScopeInit(&scope, parent);

    for (int i = f->blocks[blockIdx].start; i < f->blocks[blockIdx].end; i++) {
        svnProcessInstr(&f->instrs[i], &scope, nextVN);
    }

    for (int k = 0; k < 2; k++) {
        int s = f->blocks[blockIdx].succ[k];
        if (s >= 0 && f->blocks[s].predCount == 1) {
            svnProcessBlock(f, s, &scope, nextVN);
        }
    }

    svnScopeDestroy(&scope);
}

void svn_optimize(IRFunction *f) {
    if (f->blockCount == 0) return;
    int nextVN = 0;
    for (int i = 0; i < f->blockCount; i++) {
        if (f->blocks[i].predCount != 1) {
            svnProcessBlock(f, i, NULL, &nextVN);
        }
    }
}