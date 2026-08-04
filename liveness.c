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

/* ---- LiveSet ------------------------------------------------------------ */

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

/* ---- Motore di dataflow generico ---------------------------------------- */

LivenessBlockSets liveness_compute_core(int nBlocks, const LivenessBlock *blocks,
                                         int numVars, const char *reachable,
                                         LivenessExtractFn extract, void *ctx,
                                         Arena *arena) {
    LivenessBlockSets r;
    r.numVars = numVars;
    r.words   = (numVars + 63) / 64;

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

    int uses[LIVENESS_MAX_IDS], defs[LIVENESS_MAX_IDS], nUses, nDefs;
    for (int b = 0; b < nBlocks; b++) {
        if (reachable && !reachable[b]) continue;
        for (int i = blocks[b].start; i < blocks[b].end; i++) {
            extract(ctx, i, uses, &nUses, defs, &nDefs);
            for (int k = 0; k < nUses; k++)
                if (!liveset_test(&r.Def[b], uses[k]))
                    liveset_set(&r.Use[b], uses[k]);
            for (int k = 0; k < nDefs; k++)
                liveset_set(&r.Def[b], defs[k]);
        }
    }

    LiveSet tmp = liveset_new(arena, r.words);
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int b = nBlocks - 1; b >= 0; b--) {
            if (reachable && !reachable[b]) continue;

            liveset_clear(&r.LiveOut[b]);
            for (int k = 0; k < 2; k++) {
                int s = blocks[b].succ[k];
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

LiveSet *liveness_compute_per_instr(int nBlocks, const LivenessBlock *blocks,
                                     int instrCount, int numVars,
                                     const LiveSet *blockLiveOut,
                                     LivenessExtractFn extract, void *ctx,
                                     Arena *arena) {
    int words = (numVars + 63) / 64;

    LiveSet *liveAfter = arena_alloc(arena, (size_t)instrCount * sizeof(LiveSet));
    for (int i = 0; i < instrCount; i++) liveAfter[i] = liveset_new(arena, words);

    int uses[LIVENESS_MAX_IDS], defs[LIVENESS_MAX_IDS], nUses, nDefs;
    for (int b = 0; b < nBlocks; b++) {
        LiveSet live = liveset_new(arena, words);
        liveset_copy(&live, &blockLiveOut[b]);

        for (int i = blocks[b].end - 1; i >= blocks[b].start; i--) {
            liveset_copy(&liveAfter[i], &live);
            extract(ctx, i, uses, &nUses, defs, &nDefs);
            for (int k = 0; k < nDefs; k++) liveset_clrbit(&live, defs[k]);
            for (int k = 0; k < nUses; k++) liveset_set(&live, uses[k]);
        }
    }
    return liveAfter;
}

/* ---- Fronte IR lineare ------------------------------------------------- */

typedef struct {
    IRFunction *f;
    VarMap     *varMap;
} IRLivenessCtx;

static void irExtract(void *ctxP, int instrIdx,
                       int uses[LIVENESS_MAX_IDS], int *nUses,
                       int defs[LIVENESS_MAX_IDS], int *nDefs) {
    IRLivenessCtx *ctx = ctxP;
    IRInstr *in = &ctx->f->instrs[instrIdx];
    *nUses = 0; *nDefs = 0;

    if (liveness_is_var_or_temp(in->src1.kind)) {
        int id = varmap_operand_id(ctx->varMap, in->src1);
        if (id >= 0) uses[(*nUses)++] = id;
    }
    if (liveness_is_var_or_temp(in->src2.kind)) {
        int id = varmap_operand_id(ctx->varMap, in->src2);
        if (id >= 0) uses[(*nUses)++] = id;
    }
    if (liveness_defines_dst(in->op) && liveness_is_var_or_temp(in->dst.kind)) {
        int id = varmap_operand_id(ctx->varMap, in->dst);
        if (id >= 0) defs[(*nDefs)++] = id;
    }
}

LivenessResult liveness_compute(IRFunction *f, const char *reachable, Arena *arena) {
    int nBlocks = f->blockCount;

    LivenessResult r;
    varmap_init(&r.varMap);
    for (int i = 0; i < f->count; i++) {
        IRInstr *in = &f->instrs[i];
        varmap_operand_id(&r.varMap, in->dst);
        varmap_operand_id(&r.varMap, in->src1);
        varmap_operand_id(&r.varMap, in->src2);
    }
    r.numVars = r.varMap.nextId;

    LivenessBlock *lb = arena_alloc(arena, (size_t)nBlocks * sizeof(LivenessBlock));
    for (int b = 0; b < nBlocks; b++) {
        lb[b].start   = f->blocks[b].start;
        lb[b].end     = f->blocks[b].end;
        lb[b].succ[0] = f->blocks[b].succ[0];
        lb[b].succ[1] = f->blocks[b].succ[1];
    }

    IRLivenessCtx ctx = { f, &r.varMap };
    LivenessBlockSets bsets = liveness_compute_core(nBlocks, lb, r.numVars, reachable,
                                                      irExtract, &ctx, arena);
    r.Use = bsets.Use; r.Def = bsets.Def;
    r.LiveIn = bsets.LiveIn; r.LiveOut = bsets.LiveOut;
    r.words = bsets.words;
    return r;
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