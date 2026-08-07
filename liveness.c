#include <stdlib.h>
#include <string.h>
#include "liveness.h"

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

/* ---- Motore di dataflow generico ----------------------------------------
 *
 * Riceve BasicBlock* (ex LivenessBlock*): stesso layout, tipo unificato.
 * -------------------------------------------------------------------------*/

LivenessBlockSets liveness_compute_core(int nBlocks, const BasicBlock *blocks,
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

    size_t   wordsPerBlock = (size_t)r.words;
    size_t   totalWords    = 4 * (size_t)nBlocks * wordsPerBlock;
    uint64_t *allBits      = arena_alloc(arena, totalWords * sizeof(uint64_t));
    memset(allBits, 0, totalWords * sizeof(uint64_t));

    for (int b = 0; b < nBlocks; b++) {
        r.Use[b]     = (LiveSet){ allBits + (0 * nBlocks + b) * wordsPerBlock, r.words };
        r.Def[b]     = (LiveSet){ allBits + (1 * nBlocks + b) * wordsPerBlock, r.words };
        r.LiveIn[b]  = (LiveSet){ allBits + (2 * nBlocks + b) * wordsPerBlock, r.words };
        r.LiveOut[b] = (LiveSet){ allBits + (3 * nBlocks + b) * wordsPerBlock, r.words };
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

LiveSet *liveness_compute_per_instr(int nBlocks, const BasicBlock *blocks,
                                     int instrCount, int numVars,
                                     const LiveSet *blockLiveOut,
                                     LivenessExtractFn extract, void *ctx,
                                     Arena *arena) {
    int words = (numVars + (BITS_PER_WORD - 1)) / BITS_PER_WORD;

    LiveSet  *liveAfter = arena_alloc(arena, (size_t)instrCount * sizeof(LiveSet));
    uint64_t *allBits   = arena_alloc(arena, (size_t)instrCount * (size_t)words * sizeof(uint64_t));
    memset(allBits, 0, (size_t)instrCount * (size_t)words * sizeof(uint64_t));

    for (int i = 0; i < instrCount; i++)
        liveAfter[i] = (LiveSet){ allBits + (size_t)i * (size_t)words, words };

    int uses[LIVENESS_MAX_IDS], defs[LIVENESS_MAX_IDS], nUses, nDefs;

    uint64_t *liveBits = arena_alloc(arena, (size_t)words * sizeof(uint64_t));
    LiveSet   live     = { liveBits, words };

    for (int b = 0; b < nBlocks; b++) {
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

/* ---- Fronte IR lineare (DCE/LICM/SR) ------------------------------------ */

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

LivenessResult liveness_compute_ir(IRFunction *f, const char *reachable, Arena *arena) {
    LivenessResult r = {0};

    varmap_init(&r.varMap);
    for (int i = 0; i < f->count; i++) {
        IRInstr *in = &f->instrs[i];
        varmap_operand_id(&r.varMap, in->dst);
        varmap_operand_id(&r.varMap, in->src1);
        varmap_operand_id(&r.varMap, in->src2);
    }
    int numVars = r.varMap.nextId;

    /* Costruisce array BasicBlock dal CFG IR (IRBlock → BasicBlock via campo bb) */
    BasicBlock *lb = arena_alloc(arena, (size_t)f->blockCount * sizeof(BasicBlock));
    for (int b = 0; b < f->blockCount; b++) {
        lb[b]   = f->blocks[b].bb;
    }

    IRLivenessCtx ctx = { f, &r.varMap };
    r.blockSets = liveness_compute_core(f->blockCount, lb, numVars, reachable,
                                         irExtract, &ctx, arena);
    r.liveAfter = NULL;
    return r;
}

/* ---- Fronte codice macchina (regalloc/interference) ---------------------- */

typedef struct {
    const MachFunction *f;
    int                  nextVreg;
} MachLivenessCtx;

static void machExtract(void *ctxP, int instrIdx,
                         int uses[LIVENESS_MAX_IDS], int *nUses,
                         int defs[LIVENESS_MAX_IDS], int *nDefs) {
    MachLivenessCtx *ctx = ctxP;
    const MachInstr *in  = &ctx->f->instrs[instrIdx];
    int tmp[LIVENESS_MAX_IDS], n;
    *nUses = 0; *nDefs = 0;

    instr_uses(in, ctx->nextVreg, tmp, &n);
    for (int i = 0; i < n; i++) uses[(*nUses)++] = tmp[i];
    instr_implicit_uses(in, ctx->nextVreg, tmp, &n);
    for (int i = 0; i < n; i++) uses[(*nUses)++] = tmp[i];

    instr_defs(in, ctx->nextVreg, tmp, &n);
    for (int i = 0; i < n; i++) defs[(*nDefs)++] = tmp[i];
    instr_implicit_defs(in, ctx->nextVreg, tmp, &n);
    for (int i = 0; i < n; i++) defs[(*nDefs)++] = tmp[i];
}

/* Riceve BasicBlock* (ex RBlock*): layout identico, tipo unificato */
LivenessResult liveness_compute_mach(const MachFunction *f, const BasicBlock *blocks,
                                      int nBlocks, Arena *arena) {
    LivenessResult r = {0};

    MachLivenessCtx ctx = { f, f->nextVreg };
    int numVars = f->nextVreg + PHYS_ALLOCATABLE;

    r.blockSets = liveness_compute_core(nBlocks, blocks, numVars, NULL,
                                         machExtract, &ctx, arena);
    r.liveAfter = liveness_compute_per_instr(nBlocks, blocks, f->count, numVars,
                                              r.blockSets.LiveOut, machExtract, &ctx, arena);
    return r;
}

/* ---- Predicati (fronte IR) ---------------------------------------------- */

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