#include "varmap.h"

unsigned long varmap_hash(const void *key, size_t keySize) {
    (void)keySize;
    uint64_t v = *(const uint64_t *)key;
    v ^= v >> 33; v *= 0xff51afd7ed558ccdULL;
    v ^= v >> 33; v *= 0xc4ceb9fe1a85ec53ULL;
    v ^= v >> 33;
    return (unsigned long)v;
}

uint64_t varmap_make_key(int kind, int a, int b) {
    uint64_t k = 0;
    k |= (uint64_t)(kind & 0x3)        << 62;
    k |= (uint64_t)(a    & 0x7fffffff) << 31;
    k |= (uint64_t)(b    & 0x7fffffff);
    return k;
}

int varmap_id(VarMap *m, int kind, int a, int b) {
    uint64_t key = varmap_make_key(kind, a, b);
    int id;
    if (ht_get(m->table, &key, sizeof key, &id, sizeof id)) return id;
    id = m->nextId++;
    ht_set(m->table, &key, sizeof key, &id, sizeof id);
    return id;
}

int varmap_operand_id(VarMap *m, Operand op) {
    if (op.kind == OPND_VAR)  return varmap_id(m, 0, op.data.varLevel, op.data.varOffset);
    if (op.kind == OPND_TEMP) return varmap_id(m, 1, op.data.tempId, 0);
    return -1;
}

void varmap_init(VarMap *m) {
    m->table  = ht_create(32, varmap_hash);
    m->nextId = 0;
}

void varmap_destroy(VarMap *m) {
    ht_destroy(m->table, NULL);
}