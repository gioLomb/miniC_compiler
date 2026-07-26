#include <stdlib.h>
#include <string.h>
#include "liveness.h"

/* ---- VarMap ------------------------------------------------------------ */

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

/* ---- LiveSet ----------------------------------------------------------- */

LiveSet liveset_new(Arena *arena, int words) {
    LiveSet s;
    s.words = words;
    s.bits  = arena_alloc(arena, (size_t)words * sizeof(uint64_t));
    memset(s.bits, 0, (size_t)words * sizeof(uint64_t));
    return s;
}

void liveset_clear(LiveSet *s) {
    memset(s->bits, 0, (size_t)s->words * sizeof(uint64_t));
}

void liveset_set(LiveSet *s, int id) {
    s->bits[id >> 6] |= 1ULL << (id & 63);
}

void liveset_clrbit(LiveSet *s, int id) {
    s->bits[id >> 6] &= ~(1ULL << (id & 63));
}

int liveset_test(const LiveSet *s, int id) {
    return (s->bits[id >> 6] >> (id & 63)) & 1ULL;
}

void liveset_union(LiveSet *dst, const LiveSet *src) {
    for (int i = 0; i < dst->words; i++) dst->bits[i] |= src->bits[i];
}

void liveset_union_into(LiveSet *dst, const LiveSet *a, const LiveSet *b) {
    for (int i = 0; i < dst->words; i++) dst->bits[i] = a->bits[i] | b->bits[i];
}

void liveset_diff(LiveSet *dst, const LiveSet *a, const LiveSet *b) {
    for (int i = 0; i < dst->words; i++) dst->bits[i] = a->bits[i] & ~b->bits[i];
}

int liveset_equal(const LiveSet *a, const LiveSet *b) {
    for (int i = 0; i < a->words; i++)
        if (a->bits[i] != b->bits[i]) return 0;
    return 1;
}

void liveset_copy(LiveSet *dst, const LiveSet *src) {
    memcpy(dst->bits, src->bits, (size_t)src->words * sizeof(uint64_t));
}

/* ---- Predicati --------------------------------------------------------- */

int liveness_defines_dst(IROp op) {
    switch (op) {
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_NEG: case IR_NOT:
    case IR_LT:  case IR_LE:  case IR_GT:  case IR_GE:  case IR_EQ: case IR_NE:
    case IR_ASSIGN: case IR_LOAD_ARR: case IR_CALL:
        return 1;
    default:
        return 0;
    }
}

int liveness_is_var_or_temp(OperandKind kind) {
    return kind == OPND_VAR || kind == OPND_TEMP;
}

/* ---- liveness_compute -------------------------------------------------- */

LivenessResult liveness_compute(IRFunction *f, const char *reachable, Arena *arena) {
    int nBlocks = f->blockCount;
    int nInstrs = f->count;

    LivenessResult r;

    /* PASSO 1: costruisci VarMap scansionando tutte le istruzioni */
    varmap_init(&r.varMap);
    for (int i = 0; i < nInstrs; i++) {
        IRInstr *in = &f->instrs[i];
        varmap_operand_id(&r.varMap, in->dst);
        varmap_operand_id(&r.varMap, in->src1);
        varmap_operand_id(&r.varMap, in->src2);
    }
    r.numVars = r.varMap.nextId;
    r.words   = (r.numVars + 63) / 64;

    /* PASSO 2: alloca Use, Def, LiveIn, LiveOut nell'arena */
    r.Use     = arena_alloc(arena, (size_t)nBlocks * sizeof(LiveSet));
    r.Def     = arena_alloc(arena, (size_t)nBlocks * sizeof(LiveSet));
    r.LiveIn  = arena_alloc(arena, (size_t)nBlocks * sizeof(LiveSet));
    r.LiveOut = arena_alloc(arena, (size_t)nBlocks * sizeof(LiveSet));
    for (int b = 0; b < nBlocks; b++) {
        r.Use[b]     = liveset_new(arena, r.words);
        r.Def[b]     = liveset_new(arena, r.words);
        r.LiveIn[b]  = liveset_new(arena, r.words);
        r.LiveOut[b] = liveset_new(arena, r.words);
    }

    /* PASSO 3: calcola Use e Def per blocco
     * Use[b]: variabili usate prima di essere definite nel blocco
     *         (upward-exposed uses — le uniche rilevanti per il dataflow).
     * Def[b]: variabili definite nel blocco. */
    for (int b = 0; b < nBlocks; b++) {
        if (reachable && !reachable[b]) continue;
        for (int i = f->blocks[b].start; i < f->blocks[b].end; i++) {
            IRInstr *in = &f->instrs[i];

            if (liveness_is_var_or_temp(in->src1.kind)) {
                int id = varmap_operand_id(&r.varMap, in->src1);
                if (id >= 0 && !liveset_test(&r.Def[b], id))
                    liveset_set(&r.Use[b], id);
            }
            if (liveness_is_var_or_temp(in->src2.kind)) {
                int id = varmap_operand_id(&r.varMap, in->src2);
                if (id >= 0 && !liveset_test(&r.Def[b], id))
                    liveset_set(&r.Use[b], id);
            }
            if (liveness_defines_dst(in->op) && liveness_is_var_or_temp(in->dst.kind)) {
                int id = varmap_operand_id(&r.varMap, in->dst);
                if (id >= 0) liveset_set(&r.Def[b], id);
            }
        }
    }

    /* PASSO 4: backward dataflow a punto fisso
     * LiveOut[b] = union di LiveIn[succ]
     * LiveIn[b]  = Use[b] ∪ (LiveOut[b] - Def[b]) */
    LiveSet tmp = liveset_new(arena, r.words);
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int b = nBlocks - 1; b >= 0; b--) {
            if (reachable && !reachable[b]) continue;

            liveset_clear(&r.LiveOut[b]);
            for (int k = 0; k < 2; k++) {
                int s = f->blocks[b].succ[k];
                if (s >= 0 && s < nBlocks && !(reachable && !reachable[s]))
                    liveset_union(&r.LiveOut[b], &r.LiveIn[s]);
            }

            liveset_diff(&tmp, &r.LiveOut[b], &r.Def[b]);
            liveset_union_into(&tmp, &tmp, &r.Use[b]);

            if (!liveset_equal(&r.LiveIn[b], &tmp)) {
                liveset_copy(&r.LiveIn[b], &tmp);
                changed = 1;
            }
        }
    }

    return r;
}
