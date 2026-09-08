/**
 * @file liveness.c
 * @brief Liveness analysis engine — implementation (refactored).
 */

#include <stdlib.h>
#include <string.h>
#include "liveness.h"

static inline int liveness_words_for(int numVars) {
    return (numVars + 63) / 64;
}

/* -------------------------------------------------------------------------
 * Helper: allocates all four LiveSet header arrays and the contiguous
 * bit‑word slab, then points each LiveSet's `bits` field into the correct
 * section of the slab. Returns the number of words per set.
 * ------------------------------------------------------------------------- */
static int liveness_init_sets(int nBlocks, int numVars, LivenessBlockSets *r,
                              Arena *arena) {
    r->numVars = numVars;
    r->words = liveness_words_for(numVars);
    size_t wordsPerBlock = (size_t)r->words;

    // Allocate array-of-LiveSet headers (pointers + word counts) from arena.
    r->Use     = arena_alloc(arena, (size_t)nBlocks * sizeof(LiveSet));
    r->Def     = arena_alloc(arena, (size_t)nBlocks * sizeof(LiveSet));
    r->LiveIn  = arena_alloc(arena, (size_t)nBlocks * sizeof(LiveSet));
    r->LiveOut = arena_alloc(arena, (size_t)nBlocks * sizeof(LiveSet));

    // Single contiguous slab for all bit words: 4 arrays × nBlocks × r.words.
    size_t totalWords = 4 * (size_t)nBlocks * wordsPerBlock;
    uint64_t *slab = arena_alloc(arena, totalWords * sizeof(uint64_t));
    memset(slab, 0, totalWords * sizeof(uint64_t)); // start all sets empty

    // Partition the slab among the four set arrays.
    // Each LiveSet[b].bits points into the appropriate section of slab[].
    for (int b = 0; b < nBlocks; b++) {
        r->Use[b]     = (LiveSet){ slab + (0 * nBlocks + b) * wordsPerBlock, r->words };
        r->Def[b]     = (LiveSet){ slab + (1 * nBlocks + b) * wordsPerBlock, r->words };
        r->LiveIn[b]  = (LiveSet){ slab + (2 * nBlocks + b) * wordsPerBlock, r->words };
        r->LiveOut[b] = (LiveSet){ slab + (3 * nBlocks + b) * wordsPerBlock, r->words };
    }
    return r->words;
}


static void liveness_build_def_use(int nBlocks, const BasicBlock *blocks,
                                   const char *reachable,
                                   LivenessExtractFn extract, void *ctx,
                                   LivenessBlockSets *r) {
    int uses[LIVENESS_MAX_IDS], defs[LIVENESS_MAX_IDS], nUses, nDefs;

    for (int b = 0; b < nBlocks; b++) {
        if (reachable && !reachable[b]) continue; // skip unreachable blocks

        for (int i = blocks[b].range.start; i < blocks[b].range.end; i++) {
            extract(ctx, i, uses, &nUses, defs, &nDefs);

            // A use counts towards Use[b] only if the variable has not yet
            // been defined earlier in this block
            for (int k = 0; k < nUses; k++)
                if (!bitset_test(&r->Def[b], uses[k]))
                    bitset_set(&r->Use[b], uses[k]);

            
            for (int k = 0; k < nDefs; k++)
                bitset_set(&r->Def[b], defs[k]);
        }
    }
}


static void liveness_backward_fixedpoint(int nBlocks, const BasicBlock *blocks,
                                         const char *reachable,
                                         LivenessBlockSets *r, Arena *arena) {

    LiveSet tmp = bitset_new(arena, r->words);
    int changed = 1;

    while (changed) {
        changed = 0;

        for (int b = nBlocks - 1; b >= 0; b--) {
            if (reachable && !reachable[b]) continue;

            // LiveOut[b] = union of LiveIn[s] for all reachable successors s.
            bitset_clear(&r->LiveOut[b]);
            for (int k = 0; k < 2; k++) {
                int s = blocks[b].succ[k];
                if (s >= 0 && s < nBlocks && !(reachable && !reachable[s]))
                    bitset_or(&r->LiveOut[b], &r->LiveIn[s]);
            }

            // LiveIn[b] = Use[b] ∪ (LiveOut[b] − Def[b])
            // Compute into tmp first to detect whether anything changed.
            bitset_diff(&tmp, &r->LiveOut[b], &r->Def[b]); 
            bitset_union_into(&tmp, &tmp, &r->Use[b]);     

            if (!bitset_equal(&r->LiveIn[b], &tmp)) {
                bitset_copy(&r->LiveIn[b], &tmp);
                changed = 1; // LiveIn grew — need another iteration
            }
        }
    }
}


LivenessBlockSets liveness_computeCore(int nBlocks, const BasicBlock *blocks,
                                         int numVars, const char *reachable,
                                         LivenessExtractFn extract, void *ctx,
                                         Arena *arena) {
    LivenessBlockSets r;

    liveness_init_sets(nBlocks, numVars, &r, arena);
    liveness_build_def_use(nBlocks, blocks, reachable, extract, ctx, &r);
    liveness_backward_fixedpoint(nBlocks, blocks, reachable, &r, arena);

    return r;
}


static LiveSet *liveness_init_per_instr(int instrCount, int words, Arena *arena) {
    // Allocate liveAfter[] headers and a contiguous bit-word slab.
    LiveSet  *liveAfter = arena_alloc(arena, (size_t)instrCount * sizeof(LiveSet));
    uint64_t *slab      = arena_alloc(arena,
                                      (size_t)instrCount * (size_t)words * sizeof(uint64_t));
    memset(slab, 0, (size_t)instrCount * (size_t)words * sizeof(uint64_t));

    // Point each liveAfter[i] bits into its slice of the slab.
    for (int i = 0; i < instrCount; i++)
        liveAfter[i] = (LiveSet){ slab + (size_t)i * (size_t)words, words };

    return liveAfter;
}


static void liveness_per_instr_sweep_block(const BasicBlock *block,
                                           const LiveSet *blockLiveOut,
                                           LivenessExtractFn extract, void *ctx,
                                           LiveSet *live, LiveSet *liveAfter) {
    // Seed the sweep with the block's fixed-point live-out.
    bitset_copy(live, blockLiveOut);

    // Scan instructions in reverse order within the block.
    for (int i = block->range.end - 1; i >= block->range.start; i--) {
        // Record what is live just after instruction i runs.
        bitset_copy(&liveAfter[i], live);

        int uses[LIVENESS_MAX_IDS], defs[LIVENESS_MAX_IDS], nUses, nDefs;
        extract(ctx, i, uses, &nUses, defs, &nDefs);

        // Backward transfer: kill definitions first, then add uses.
        for (int k = 0; k < nDefs; k++) bitset_clr(live, defs[k]);
        for (int k = 0; k < nUses; k++) bitset_set(live, uses[k]);
    }
}


LiveSet *liveness_computePerInstr(int nBlocks, const BasicBlock *blocks,
                                     int instrCount, int numVars,
                                     const LiveSet *blockLiveOut,
                                     LivenessExtractFn extract, void *ctx,
                                     Arena *arena) {
    int words = liveness_words_for(numVars);
    LiveSet *liveAfter = liveness_init_per_instr(instrCount, words, arena);

    // Scratch live set reused for each block's backward sweep.
    uint64_t *liveBits = arena_alloc(arena, (size_t)words * sizeof(uint64_t));
    LiveSet   live     = { liveBits, words };

    for (int b = 0; b < nBlocks; b++) {
        liveness_per_instr_sweep_block(&blocks[b], &blockLiveOut[b],
                                       extract, ctx, &live, liveAfter);
    }
    return liveAfter;
}


/** Context passed from liveness_computeIr to irExtract. */
// typedef struct {
//     IRFunction *f;
//     VarMap     *varMap;
// } IRLivenessCtx;

/**
 * @brief Extract use and def ids from IR instruction at @p instrIdx.
 *
 * Dispatch rules:
 *   - src1, src2 are always uses if they are storage operands.
 *   - dst of IR_STORE_ARR is a use (the array base address is *read* to
 *     compute the effective address, not written); this case requires special
 *     handling because ir_defines_dst() returns false for IR_STORE_ARR.
 *   - dst of any instruction where ir_defines_dst() is true is a def.
 *
 * Non-storage operands (constants, labels, function names) are silently
 * ignored — they have no VarMap id and cannot be live.
 */
// static void irExtract(void *ctxP, int instrIdx,
//                        int uses[LIVENESS_MAX_IDS], int *nUses,
//                        int defs[LIVENESS_MAX_IDS], int *nDefs) {
//     IRLivenessCtx *ctx = ctxP;
//     IRInstr *in = &ctx->f->instrs[instrIdx];
//     *nUses = 0; *nDefs = 0;

//     // src1 and src2 are always source operands — add them as uses.
//     if (ir_operand_is_storage(in->src1.kind)) {
//         int id = varmap_operand_id(ctx->varMap, in->src1);
//         if (id >= 0) uses[(*nUses)++] = id;
//     }
//     if (ir_operand_is_storage(in->src2.kind)) {
//         int id = varmap_operand_id(ctx->varMap, in->src2);
//         if (id >= 0) uses[(*nUses)++] = id;
//     }

//     // case IR_STORE_ARR  dst[src1] = src2

//     if (!ir_defines_dst(in->op) && ir_operand_is_storage(in->dst.kind)) {
//         int id = varmap_operand_id(ctx->varMap, in->dst);
//         if (id >= 0) uses[(*nUses)++] = id;
//     }

//     // For all opcodes that write a result into dst, dst is a def.
//     if (ir_defines_dst(in->op) && ir_operand_is_storage(in->dst.kind)) {
//         int id = varmap_operand_id(ctx->varMap, in->dst);
//         if (id >= 0) defs[(*nDefs)++] = id;
//     }
// }


static void ir_populate_varmap(IRFunction *f, VarMap *varMap) {
    for (int i = 0; i < f->count; i++) {
        IRInstr *in = &f->instrs[i];
        varmap_operand_id(varMap, in->dst);
        varmap_operand_id(varMap, in->src1);
        varmap_operand_id(varMap, in->src2);
    }
}


static BasicBlock *ir_convert_blocks(IRFunction *f, Arena *arena) {
    BasicBlock *lb = arena_alloc(arena, (size_t)f->blockCount * sizeof(BasicBlock));
    for (int b = 0; b < f->blockCount; b++)
        lb[b] = f->blocks[b].bb;
    return lb;
}

/** Context passed from liveness_computeIr to irExtract. */
typedef struct {
    IRFunction *f;
    VarMap     *vm;
} IRLivenessCtx;

static void irExtract(void *ctxP, int instrIdx,
                       int uses[LIVENESS_MAX_IDS], int *nUses,
                       int defs[LIVENESS_MAX_IDS], int *nDefs) {
    IRLivenessCtx *ctx = ctxP;
    IRInstr *in = &ctx->f->instrs[instrIdx];
    VarMap  *vm = ctx->vm;
    *nUses = 0; *nDefs = 0;

    // varmap_operand_id() already returns -1 for non-storage kinds and
    // resolves through VarMap's own internal cache: no per-instruction
    // id array needed here anymore.
    int s1 = varmap_operand_id(vm, in->src1);
    int s2 = varmap_operand_id(vm, in->src2);
    int d  = varmap_operand_id(vm, in->dst);

    if (s1 >= 0) uses[(*nUses)++] = s1;
    if (s2 >= 0) uses[(*nUses)++] = s2;
    if (!ir_defines_dst(in->op) && d >= 0) uses[(*nUses)++] = d;
    if (ir_defines_dst(in->op)  && d >= 0) defs[(*nDefs)++] = d;
}
/* ir_populate_varmap() rimossa: sostituita da varmap_sync_cache(). */

LivenessResult liveness_computeIr(IRFunction *f, const char *reachable,
                                    VarMap *sharedVarMap, Arena *arena) {
    LivenessResult r = {0};

    VarMap  owned;
    VarMap *vm = sharedVarMap;
    if (!vm) { owned = varmap_init(); vm = &owned; }

    // Prescan: register every operand of f (get-or-create) so numVars
    // below is final before the bitsets are sized.
    ir_populate_varmap(f, vm);

    int numVars = vm->nextId;
    BasicBlock *lb = ir_convert_blocks(f, arena);

    IRLivenessCtx ctx = { f, vm };
    r.blockSets = liveness_computeCore(f->blockCount, lb, numVars, reachable,
                                         irExtract, &ctx, arena);
    r.liveAfter = NULL;
    r.varMap    = *vm;
    return r;
}
/* 
 * Machine-code front-end
*/

/** Context passed from liveness_computeMach to machExtract. */
typedef struct {
    const MachFunction *f;
    int   nextVreg; // base offset: phys reg P → id nextVreg + P
} MachLivenessCtx;

/**
 * @brief Extract use and def ids from machine instruction at @p instrIdx.
 *
 * Combines explicit operand uses/defs (via instr_uses / instr_defs) with
 * implicit ABI reads and writes (via instr_implicit_uses / instr_implicit_defs).
 * Examples of implicit traffic:
 *   - CALL: reads all argument registers (rdi, rsi, ...),
 *           writes RAX (return value) and clobbers all caller-saved regs.
 *   - IDIV: reads RAX and RDX (dividend), writes RAX (quotient) and RDX (remainder).
 *   - CQO:  reads RAX, writes RDX (sign extension).
 */
static void machExtract(void *ctxP, int instrIdx,
                         int uses[LIVENESS_MAX_IDS], int *nUses,
                         int defs[LIVENESS_MAX_IDS], int *nDefs) {
    MachLivenessCtx *ctx = ctxP;
    const MachInstr *in  = &ctx->f->instrs[instrIdx];
    int tmp[LIVENESS_MAX_IDS], n;
    *nUses = 0; *nDefs = 0;

    // Collect explicit uses (src1, src2 registers + RMW dsts like STORE base).
    instr_uses(in, ctx->nextVreg, tmp, &n);
    for (int i = 0; i < n; i++) uses[(*nUses)++] = tmp[i];

    // Collect implicit ABI uses (e.g. argument registers before CALL).
    instr_implicit_uses(in, ctx->nextVreg, tmp, &n);
    for (int i = 0; i < n; i++) uses[(*nUses)++] = tmp[i];

    // Collect explicit defs (the single dst register written, if any).
    instr_defs(in, ctx->nextVreg, tmp, &n);
    for (int i = 0; i < n; i++) defs[(*nDefs)++] = tmp[i];

    // Collect implicit ABI defs
    instr_implicit_defs(in, ctx->nextVreg, tmp, &n);
    for (int i = 0; i < n; i++) defs[(*nDefs)++] = tmp[i];
}

LivenessResult liveness_computeMach(const MachFunction *f,
                                      const BasicBlock *blocks,
                                      int nBlocks, Arena *arena) {
    LivenessResult r = {0};
    MachLivenessCtx ctx = { f, f->nextVreg };
    int numVars = f->nextVreg + PHYS_ALLOCATABLE;

    // Run the backward dataflow engine
    r.blockSets = liveness_computeCore(nBlocks, blocks, numVars, NULL,
                                         machExtract, &ctx, arena);

    // Compute per-instruction liveAfter[] needed by ig_build().
    // This is a single additional backward sweep seeded with the fixed-point
    // LiveOut[] from the engine above.
    r.liveAfter = liveness_computePerInstr(nBlocks, blocks, f->count, numVars,
                                              r.blockSets.LiveOut,
                                              machExtract, &ctx, arena);
    return r;
}