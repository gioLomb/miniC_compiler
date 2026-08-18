/**
 * @file liveness.c
 * @brief Liveness analysis engine — implementation.
 *
 * See liveness.h for the module overview and public API documentation.
 *
 * Internal organisation
 * ---------------------
 *  1. liveness_computeCore     — generic backward dataflow engine.
 *  2. liveness_computePerInstr — single backward sweep for per-instruction sets.
 *  3. IR front-end             — irExtract callback + liveness_computeIr wrapper.
 *  4. Machine front-end        — machExtract callback + liveness_computeMach wrapper.
 *
 * Note: bit-set primitives live in bitset.h (all inline); LiveSet is a
 * typedef of BitSet defined in liveness.h.
 */

#include <stdlib.h>
#include <string.h>
#include "liveness.h"

/* =========================================================================
 * Generic backward dataflow engine
 * =========================================================================
 *
 * Algorithm (standard backward liveness, iterative to fixed point):
 *
 *   for each block b (forward pass to build Use/Def):
 *     scan instructions b.start..b.end-1 via extract():
 *       Use[b]  |= use_i  (only ids not already in Def[b])
 *       Def[b]  |= def_i
 *
 *   repeat until no LiveIn set changes (backward pass):
 *     for b = nBlocks-1 downto 0:
 *       LiveOut[b] = union( LiveIn[s] for s in succ[b] )
 *       LiveIn[b]  = Use[b] ∪ (LiveOut[b] − Def[b])
 *
 * Memory layout: all LiveSet bit words for all four set arrays (Use, Def,
 * LiveIn, LiveOut) are allocated as a single contiguous slab from the
 * arena, improving cache behaviour during the iterative fixed-point loop.
 * ========================================================================= */

LivenessBlockSets liveness_computeCore(int nBlocks, const BasicBlock *blocks,
                                         int numVars, const char *reachable,
                                         LivenessExtractFn extract, void *ctx,
                                         Arena *arena) {
    LivenessBlockSets r;
    r.numVars = numVars;
    r.words   = (numVars + 63) / 64;

    // Allocate the four per-block set arrays (headers only; bits come below).
    r.Use     = arena_alloc(arena, (size_t)nBlocks * sizeof(LiveSet));
    r.Def     = arena_alloc(arena, (size_t)nBlocks * sizeof(LiveSet));
    r.LiveIn  = arena_alloc(arena, (size_t)nBlocks * sizeof(LiveSet));
    r.LiveOut = arena_alloc(arena, (size_t)nBlocks * sizeof(LiveSet));

    // Single slab for all bit words: 4 arrays × nBlocks blocks × r.words uint64_t.
    size_t   wordsPerBlock = (size_t)r.words;
    size_t   totalWords    = 4 * (size_t)nBlocks * wordsPerBlock;
    uint64_t *allBits      = arena_alloc(arena, totalWords * sizeof(uint64_t));
    memset(allBits, 0, totalWords * sizeof(uint64_t));

    // Partition the slab among the four set arrays.
    for (int b = 0; b < nBlocks; b++) {
        r.Use[b]     = (LiveSet){ allBits + (0 * nBlocks + b) * wordsPerBlock, r.words };
        r.Def[b]     = (LiveSet){ allBits + (1 * nBlocks + b) * wordsPerBlock, r.words };
        r.LiveIn[b]  = (LiveSet){ allBits + (2 * nBlocks + b) * wordsPerBlock, r.words };
        r.LiveOut[b] = (LiveSet){ allBits + (3 * nBlocks + b) * wordsPerBlock, r.words };
    }

    // --- Forward pass: build Use and Def for each reachable block. ---
    int uses[LIVENESS_MAX_IDS], defs[LIVENESS_MAX_IDS], nUses, nDefs;
    for (int b = 0; b < nBlocks; b++) {
        if (reachable && !reachable[b]) continue;
        for (int i = blocks[b].start; i < blocks[b].end; i++) {
            extract(ctx, i, uses, &nUses, defs, &nDefs);
            // A use counts only if the variable has not yet been defined in this block.
            for (int k = 0; k < nUses; k++)
                if (!bitset_test(&r.Def[b], uses[k]))
                    bitset_set(&r.Use[b], uses[k]);
            for (int k = 0; k < nDefs; k++)
                bitset_set(&r.Def[b], defs[k]);
        }
    }

    // --- Backward iterative fixed-point. ---
    LiveSet tmp = bitset_new(arena, r.words);
    int changed = 1;
    while (changed) {
        changed = 0;
        // Scanning blocks in reverse order accelerates convergence for loops.
        for (int b = nBlocks - 1; b >= 0; b--) {
            if (reachable && !reachable[b]) continue;

            // LiveOut[b] = union of LiveIn[s] for all successors s.
            bitset_clear(&r.LiveOut[b]);
            for (int k = 0; k < 2; k++) {
                int s = blocks[b].succ[k];
                if (s >= 0 && s < nBlocks && !(reachable && !reachable[s]))
                    bitset_or(&r.LiveOut[b], &r.LiveIn[s]);
            }

            // LiveIn[b] = Use[b] ∪ (LiveOut[b] − Def[b]).
            bitset_diff(&tmp, &r.LiveOut[b], &r.Def[b]);
            bitset_union_into(&tmp, &tmp, &r.Use[b]);

            if (!bitset_equal(&r.LiveIn[b], &tmp)) {
                bitset_copy(&r.LiveIn[b], &tmp);
                changed = 1;
            }
        }
    }

    return r;
}

/* =========================================================================
 * Per-instruction liveness
 * =========================================================================
 * A single backward sweep (no iteration needed — block-level LiveOut is
 * already at the fixed point) produces liveAfter[i] for each instruction.
 * ========================================================================= */

LiveSet *liveness_computePerInstr(int nBlocks, const BasicBlock *blocks,
                                     int instrCount, int numVars,
                                     const LiveSet *blockLiveOut,
                                     LivenessExtractFn extract, void *ctx,
                                     Arena *arena) {
    int words = (numVars + (BITS_PER_WORD - 1)) / BITS_PER_WORD;

    // Allocate liveAfter[] headers and a contiguous bit slab.
    LiveSet  *liveAfter = arena_alloc(arena, (size_t)instrCount * sizeof(LiveSet));
    uint64_t *allBits   = arena_alloc(arena,
                              (size_t)instrCount * (size_t)words * sizeof(uint64_t));
    memset(allBits, 0, (size_t)instrCount * (size_t)words * sizeof(uint64_t));

    for (int i = 0; i < instrCount; i++)
        liveAfter[i] = (LiveSet){ allBits + (size_t)i * (size_t)words, words };

    int uses[LIVENESS_MAX_IDS], defs[LIVENESS_MAX_IDS], nUses, nDefs;

    // Scratch set reused for each block's backward sweep.
    uint64_t *liveBits = arena_alloc(arena, (size_t)words * sizeof(uint64_t));
    LiveSet   live     = { liveBits, words };

    for (int b = 0; b < nBlocks; b++) {
        // Seed the sweep with the block-level live-out.
        bitset_copy(&live, &blockLiveOut[b]);

        for (int i = blocks[b].end - 1; i >= blocks[b].start; i--) {
            bitset_copy(&liveAfter[i], &live);
            extract(ctx, i, uses, &nUses, defs, &nDefs);
            // Remove definitions, then add uses (standard backward transfer).
            for (int k = 0; k < nDefs; k++) bitset_clr(&live, defs[k]);
            for (int k = 0; k < nUses; k++) bitset_set(&live, uses[k]);
        }
    }
    return liveAfter;
}

/* =========================================================================
 * IR front-end
 * =========================================================================
 * Maps IR Operands to compact integer ids via VarMap, then calls the
 * generic engine.  Only block-level liveness is computed (liveAfter=NULL).
 * ========================================================================= */

/** Context forwarded to irExtract. */
typedef struct {
    IRFunction *f;
    VarMap     *varMap;
} IRLivenessCtx;

/**
 * @brief Extract uses and defs from IR instruction @p instrIdx.
 *
 * src1 and src2 are uses; dst is a def for opcodes that write a result.
 * Constants, labels, and function names are ignored — they have no id in
 * the VarMap.
 */

 static void irExtract(void *ctxP, int instrIdx,
                       int uses[LIVENESS_MAX_IDS], int *nUses,
                       int defs[LIVENESS_MAX_IDS], int *nDefs) {
    IRLivenessCtx *ctx = ctxP;
    IRInstr *in = &ctx->f->instrs[instrIdx];
    *nUses = 0; *nDefs = 0;

    /* src1 e src2 sono usi */
    if (ir_operand_is_storage(in->src1.kind)) {
        int id = varmap_operand_id(ctx->varMap, in->src1);
        if (id >= 0) uses[(*nUses)++] = id;
    }
    if (ir_operand_is_storage(in->src2.kind)) {
        int id = varmap_operand_id(ctx->varMap, in->src2);
        if (id >= 0) uses[(*nUses)++] = id;
    }

    /* ---- NUOVO: dst di STORE_ARR è un uso ---- */
    if (!ir_defines_dst(in->op) && ir_operand_is_storage(in->dst.kind)) {
        int id = varmap_operand_id(ctx->varMap, in->dst);
        if (id >= 0) uses[(*nUses)++] = id;
    }

    /* Definizione solo per istruzioni che scrivono in una locazione */
    if (ir_defines_dst(in->op) && ir_operand_is_storage(in->dst.kind)) {
        int id = varmap_operand_id(ctx->varMap, in->dst);
        if (id >= 0) defs[(*nDefs)++] = id;
    }
}

// static void irExtract(void *ctxP, int instrIdx,
//                        int uses[LIVENESS_MAX_IDS], int *nUses,
//                        int defs[LIVENESS_MAX_IDS], int *nDefs) {
//     IRLivenessCtx *ctx = ctxP;
//     IRInstr *in = &ctx->f->instrs[instrIdx];
//     *nUses = 0; *nDefs = 0;

//     if (ir_operand_is_storage(in->src1.kind)) {
//         int id = varmap_operand_id(ctx->varMap, in->src1);
//         if (id >= 0) uses[(*nUses)++] = id;
//     }
//     if (ir_operand_is_storage(in->src2.kind)) {
//         int id = varmap_operand_id(ctx->varMap, in->src2);
//         if (id >= 0) uses[(*nUses)++] = id;
//     }
//     if (ir_defines_dst(in->op) && ir_operand_is_storage(in->dst.kind)) {
//         int id = varmap_operand_id(ctx->varMap, in->dst);
//         if (id >= 0) defs[(*nDefs)++] = id;
//     }
// }

LivenessResult liveness_computeIr(IRFunction *f, const char *reachable,
                                    Arena *arena) {
    LivenessResult r = {0};

    // Assign a compact id to every distinct Operand that appears in the function.
    varmap_init(&r.varMap);
    for (int i = 0; i < f->count; i++) {
        IRInstr *in = &f->instrs[i];
        varmap_operand_id(&r.varMap, in->dst);
        varmap_operand_id(&r.varMap, in->src1);
        varmap_operand_id(&r.varMap, in->src2);
    }
    int numVars = r.varMap.nextId;

    // Convert IRBlock descriptors to the generic BasicBlock layout.
    BasicBlock *lb = arena_alloc(arena, (size_t)f->blockCount * sizeof(BasicBlock));
    for (int b = 0; b < f->blockCount; b++)
        lb[b] = f->blocks[b].bb;

    IRLivenessCtx ctx = { f, &r.varMap };
    r.blockSets = liveness_computeCore(f->blockCount, lb, numVars, reachable,
                                         irExtract, &ctx, arena);
    r.liveAfter = NULL;   // not needed by IR-level passes
    return r;
}

/* =========================================================================
 * Machine-code front-end
 * =========================================================================
 * Variable ids are assigned by the regalloc layer: vregs occupy [0, nextVreg)
 * and physical registers occupy [nextVreg, nextVreg + PHYS_ALLOCATABLE).
 * This front-end also computes per-instruction liveness for the interference
 * graph builder.
 * ========================================================================= */

/** Context forwarded to machExtract. */
typedef struct {
    const MachFunction *f;
    int                 nextVreg;
} MachLivenessCtx;

/**
 * @brief Extract uses and defs from machine instruction @p instrIdx.
 *
 * Combines explicit operand uses/defs with implicit ones (e.g. caller-saved
 * registers clobbered by a CALL, RAX/RDX clobbered by IDIV).
 */
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

LivenessResult liveness_computeMach(const MachFunction *f,
                                      const BasicBlock *blocks,
                                      int nBlocks, Arena *arena) {
    LivenessResult r = {0};

    // Total variable universe: vregs + physical registers.
    MachLivenessCtx ctx = { f, f->nextVreg };
    int numVars = f->nextVreg + PHYS_ALLOCATABLE;

    r.blockSets = liveness_computeCore(nBlocks, blocks, numVars, NULL,
                                         machExtract, &ctx, arena);

    // The interference-graph builder requires per-instruction liveAfter[].
    r.liveAfter = liveness_computePerInstr(nBlocks, blocks, f->count, numVars,
                                              r.blockSets.LiveOut,
                                              machExtract, &ctx, arena);
    return r;
}