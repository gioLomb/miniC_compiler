#ifndef CONSTMAP_H
#define CONSTMAP_H

#include "ir.h"
#include "varmap.h"
#include "arena.h"

/* ---- Reticolo dei valori ---- */
#define LAT_UNKNOWN   0
#define LAT_CONST     1
#define LAT_CONFLICT  2

typedef struct {
    int state;
    int isFloat;          /* 1 per costante float, 0 per int */
    union {
        int   ival;
        float fval;
    } val;
} LatVal;

/* Operazioni sul reticolo */
LatVal lat_unknown(void);
LatVal lat_const_int(int ival);
LatVal lat_const_float(float fval);
LatVal lat_conflict(void);
LatVal lat_meet(LatVal a, LatVal b);
int    lat_equal(LatVal a, LatVal b);
int    lat_is_const(LatVal v);
int    lat_is_unknown(LatVal v);
int    lat_is_conflict(LatVal v);

/* ConstMap */
typedef struct {
    LatVal *vals;
    int     size;
} ConstMap;

void    constMap_init(ConstMap *m, int size, Arena *arena);
void    constMap_copy(ConstMap *dst, const ConstMap *src);
int     constMap_equal(const ConstMap *a, const ConstMap *b);
void    constMap_meet(ConstMap *dest, const ConstMap *src);
LatVal  constMap_get(const ConstMap *m, Operand op, VarMap *vm);
Operand constMap_try_fold(Operand op, const ConstMap *m, VarMap *vm);

/* ---- Helper per transferInstr (folding) ---- */
LatVal getLatVal(const ConstMap *map, Operand op, VarMap *vm);
int    isBinaryOp(IROp op);
int    isComparisonOp(IROp op);
int    foldBinaryInt(IROp op, int a, int b, int *res);
int    foldBinaryFloat(IROp op, float a, float b, float *res);
LatVal foldUnary(IROp op, LatVal v);

#endif