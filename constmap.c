#include "constmap.h"
#include "arena.h"
#include <string.h>

/* ---- LatVal helpers ---------------------------------------------------- */
LatVal lat_unknown(void) {
    LatVal v = {0}; v.state = LAT_UNKNOWN; return v;
}
LatVal lat_const_int(int ival) {
    LatVal v = {0}; v.state = LAT_CONST; v.isFloat = 0; v.val.ival = ival; return v;
}
LatVal lat_const_float(float fval) {
    LatVal v = {0}; v.state = LAT_CONST; v.isFloat = 1; v.val.fval = fval; return v;
}
LatVal lat_conflict(void) {
    LatVal v = {0}; v.state = LAT_CONFLICT; return v;
}

int lat_is_const(LatVal v) { return v.state == LAT_CONST; }
int lat_is_unknown(LatVal v) { return v.state == LAT_UNKNOWN; }
int lat_is_conflict(LatVal v) { return v.state == LAT_CONFLICT; }

LatVal lat_meet(LatVal a, LatVal b) {
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

int lat_equal(LatVal a, LatVal b) {
    if (a.state != b.state) return 0;
    if (a.state != LAT_CONST) return 1;
    if (a.isFloat != b.isFloat) return 0;
    return a.isFloat ? (a.val.fval == b.val.fval) : (a.val.ival == b.val.ival);
}

/* ---- ConstMap ---------------------------------------------------------- */
void constMap_init(ConstMap *m, int size, Arena *arena) {
    m->size = size;
    m->vals = arena_alloc(arena, (size_t)size * sizeof(LatVal));
    for (int i = 0; i < size; i++) m->vals[i] = lat_unknown();
}

void constMap_copy(ConstMap *dst, const ConstMap *src) {
    memcpy(dst->vals, src->vals, (size_t)src->size * sizeof(LatVal));
}

int constMap_equal(const ConstMap *a, const ConstMap *b) {
    for (int i = 0; i < a->size; i++)
        if (!lat_equal(a->vals[i], b->vals[i])) return 0;
    return 1;
}

void constMap_meet(ConstMap *dest, const ConstMap *src) {
    for (int i = 0; i < dest->size; i++)
        dest->vals[i] = lat_meet(dest->vals[i], src->vals[i]);
}

LatVal constMap_get(const ConstMap *m, Operand op, VarMap *vm) {
    int id = varmap_operand_id(vm, op);
    if (id < 0 || id >= m->size) return lat_conflict();
    return m->vals[id];
}

Operand constMap_try_fold(Operand op, const ConstMap *m, VarMap *vm) {
    if (op.kind != OPND_VAR && op.kind != OPND_TEMP) return op;
    LatVal lv = constMap_get(m, op, vm);
    if (lv.state != LAT_CONST) return op;
    if (lv.isFloat) {
        Operand o; o.kind = OPND_CONST_FLOAT; o.data.floatVal = lv.val.fval; return o;
    } else {
        Operand o; o.kind = OPND_CONST_INT;   o.data.intVal   = lv.val.ival; return o;
    }
}

/* ---- Helper per transferInstr (folding) ---- */
LatVal getLatVal(const ConstMap *map, Operand op, VarMap *vm) {
    switch (op.kind) {
    case OPND_CONST_INT:   return lat_const_int(op.data.intVal);
    case OPND_CONST_FLOAT: return lat_const_float(op.data.floatVal);
    case OPND_VAR:
    case OPND_TEMP:        return constMap_get(map, op, vm);
    default:               return lat_conflict();
    }
}

int isBinaryOp(IROp op) {
    switch (op) {
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_LT:  case IR_LE:  case IR_GT:  case IR_GE:  case IR_EQ: case IR_NE:
        return 1;
    default: return 0;
    }
}

int isComparisonOp(IROp op) {
    switch (op) {
    case IR_LT: case IR_LE: case IR_GT: case IR_GE: case IR_EQ: case IR_NE:
        return 1;
    default: return 0;
    }
}

int foldBinaryInt(IROp op, int a, int b, int *res) {
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

int foldBinaryFloat(IROp op, float a, float b, float *res) {
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

LatVal foldUnary(IROp op, LatVal v) {
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