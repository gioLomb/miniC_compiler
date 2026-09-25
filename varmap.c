/**
 * @file varmap.c
 * @brief Dense operand→id map: temp array by tempId, var list by (level,offset).
 *
 * Invariant: each distinct storage operand gets one id in [0, nextId).
 * First encounter assigns nextId++; later lookups return the same id.
 * Suitable for bitset-backed analyses; no hash table or secondary cache.
 */

#include "varmap.h"
#include <stdlib.h>
#include <string.h>

struct VarMap {
    int nextId;

    /* TEMP: index by tempId → dense id, or -1 if unseen */
    int *tempToId;
    int  tempCap;

    /* VAR: parallel arrays (level, offset) → dense id; linear scan (few locals) */
    int *varLevel;
    int *varOffset;
    int *varId;
    int  varN;
    int  varCap;
};

static int ensure_temp_cap(VarMap *m, int needIndex) {
    if (needIndex < m->tempCap) return 1;
    int newCap = m->tempCap ? m->tempCap : 16;
    while (newCap <= needIndex) newCap *= 2;
    int *p = realloc(m->tempToId, (size_t)newCap * sizeof(int));
    if (!p) return 0;
    for (int i = m->tempCap; i < newCap; i++)
        p[i] = -1;
    m->tempToId = p;
    m->tempCap  = newCap;
    return 1;
}

static int ensure_var_cap(VarMap *m) {
    if (m->varN < m->varCap) return 1;
    int newCap = m->varCap ? m->varCap * 2 : 8;
    int *lv = realloc(m->varLevel,  (size_t)newCap * sizeof(int));
    int *of = realloc(m->varOffset, (size_t)newCap * sizeof(int));
    int *id = realloc(m->varId,     (size_t)newCap * sizeof(int));
    if (!lv || !of || !id) {
        free(lv); free(of); free(id);
        return 0;
    }
    m->varLevel  = lv;
    m->varOffset = of;
    m->varId     = id;
    m->varCap    = newCap;
    return 1;
}

int varmap_mapToIndex(VarMap *m, int kind, int a, int b) {
    if (kind == 1) {
        /* TEMP: a = tempId, b unused */
        if (a < 0) a = 0;
        if (!ensure_temp_cap(m, a))
            return 0; /* allocation failure: degrade to id 0 */
        if (m->tempToId[a] >= 0)
            return m->tempToId[a];
        int id = m->nextId++;
        m->tempToId[a] = id;
        return id;
    }

    /* VAR: linear search then append */
    for (int i = 0; i < m->varN; i++) {
        if (m->varLevel[i] == a && m->varOffset[i] == b)
            return m->varId[i];
    }
    if (!ensure_var_cap(m))
        return 0;
    int id = m->nextId++;
    m->varLevel[m->varN]  = a;
    m->varOffset[m->varN] = b;
    m->varId[m->varN]     = id;
    m->varN++;
    return id;
}

int varmap_operand_id(VarMap *m, Operand op) {
    if (op.kind == OPND_VAR)
        return varmap_mapToIndex(m, 0, op.data.varLevel, op.data.varOffset);
    if (op.kind == OPND_TEMP)
        return varmap_mapToIndex(m, 1, op.data.tempId, 0);
    return -1;
}

VarMap *varmap_create(void) {
    VarMap *m = calloc(1, sizeof(VarMap));
    return m;
}

int varmap_count(const VarMap *m) {
    return m->nextId;
}

void varmap_destroy(VarMap *m) {
    if (!m) return;
    free(m->tempToId);
    free(m->varLevel);
    free(m->varOffset);
    free(m->varId);
    free(m);
}
