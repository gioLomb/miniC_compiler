/**
 * @file liveness.c
 * @brief Liveness analysis engine — implementation.
 *
 * See liveness.h for the full module overview and public API documentation.
 *
 * ### Internal organisation
 *  1. liveness_computeCore     — generic backward dataflow engine.
 *  2. liveness_computePerInstr — single backward sweep for per-instruction sets.
 *  3. IR front-end             — irExtract callback + liveness_computeIr wrapper.
 *  4. Machine front-end        — machExtract callback + liveness_computeMach wrapper.
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
 *   Forward pass — build Use[b] and Def[b] for each block:
 *     For each instruction i in b (in program order):
 *       Use[b]  |= uses(i)  \ Def[b]   (only ids not yet defined in b)
 *       Def[b]  |= defs(i)
 *
 *   Backward fixed-point loop — until no LiveIn set changes:
 *     For b = nBlocks-1 downto 0:
 *       LiveOut[b] = ∪ { LiveIn[s] | s ∈ succ(b) }
 *       LiveIn[b]  = Use[b] ∪ (LiveOut[b] − Def[b])
 *
 * Scanning blocks in reverse order (nBlocks-1 downto 0) accelerates
 * convergence for reducible CFGs because it processes blocks in roughly
 * reverse-post-order, propagating live info "up" the CFG in one pass.
 *
 * Memory layout:
 *   All bit words for all four arrays (Use, Def, LiveIn, LiveOut) are
 *   allocated as a single contiguous slab:
 *     slab[0 .. nBlocks*words)       → Use  word pool
 *     slab[nBlocks*words .. 2*NW)    → Def  word pool
 *     slab[2*NW .. 3*NW)             → LiveIn word pool
 *     slab[3*NW .. 4*NW)             → LiveOut word pool
 *   where NW = nBlocks * words.
 *   The flat allocation avoids nBlocks * 4 separate malloc calls and keeps
 *   the data contiguous for cache-friendly iteration in the fixed-point loop.
 * ========================================================================= */

LivenessBlockSets liveness_computeCore(int nBlocks, const BasicBlock *blocks,
                                         int numVars, const char *reachable,
                                         LivenessExtractFn extract, void *ctx,
                                         Arena *arena) {
    LivenessBlockSets r;
    r.numVars = numVars;
    // Number of 64-bit words needed to represent one set of numVars bits.
    r.words = (numVars + 63) / 64;

    // Allocate array-of-LiveSet headers (pointers + word counts) from arena.
    r.Use     = arena_alloc(arena, (size_t)nBlocks * sizeof(LiveSet));
    r.Def     = arena_alloc(arena, (size_t)nBlocks * sizeof(LiveSet));
    r.LiveIn  = arena_alloc(arena, (size_t)nBlocks * sizeof(LiveSet));
    r.LiveOut = arena_alloc(arena, (size_t)nBlocks * sizeof(LiveSet));

    // Single contiguous slab for all bit words: 4 arrays × nBlocks × r.words.
    size_t   wordsPerBlock = (size_t)r.words;
    size_t   totalWords    = 4 * (size_t)nBlocks * wordsPerBlock;
    uint64_t *allBits      = arena_alloc(arena, totalWords * sizeof(uint64_t));
    memset(allBits, 0, totalWords * sizeof(uint64_t)); // start all sets empty

    // Partition the slab among the four set arrays.
    // Each LiveSet[b].bits points into the appropriate section of allBits[].
    for (int b = 0; b < nBlocks; b++) {
        r.Use[b]     = (LiveSet){ allBits + (0 * nBlocks + b) * wordsPerBlock, r.words };
        r.Def[b]     = (LiveSet){ allBits + (1 * nBlocks + b) * wordsPerBlock, r.words };
        r.LiveIn[b]  = (LiveSet){ allBits + (2 * nBlocks + b) * wordsPerBlock, r.words };
        r.LiveOut[b] = (LiveSet){ allBits + (3 * nBlocks + b) * wordsPerBlock, r.words };
    }

    /* --- Forward pass: build Use[b] and Def[b] --- */
    int uses[LIVENESS_MAX_IDS], defs[LIVENESS_MAX_IDS], nUses, nDefs;
    for (int b = 0; b < nBlocks; b++) {
        if (reachable && !reachable[b]) continue; // skip unreachable blocks

        for (int i = blocks[b].range.start; i < blocks[b].range.end; i++) {
            extract(ctx, i, uses, &nUses, defs, &nDefs);

            // A use counts towards Use[b] only if the variable has not yet
            // been defined earlier in this block (standard upward-exposed-use
            // definition: only the first use before any local def matters).
            for (int k = 0; k < nUses; k++)
                if (!bitset_test(&r.Def[b], uses[k]))
                    bitset_set(&r.Use[b], uses[k]);

            // All defs in the block contribute to Def[b].
            for (int k = 0; k < nDefs; k++)
                bitset_set(&r.Def[b], defs[k]);
        }
    }

    /* --- Backward fixed-point loop --- */
    // Scratch set reused every iteration to compute a candidate LiveIn value
    // without clobbering the previous one (need old value to detect change).
    LiveSet tmp = bitset_new(arena, r.words);
    int changed = 1;
    while (changed) {
        changed = 0;
        // Reverse order: gives roughly one-pass convergence on reducible CFGs
        // by processing each block after its successors.
        for (int b = nBlocks - 1; b >= 0; b--) {
            if (reachable && !reachable[b]) continue;

            // LiveOut[b] = union of LiveIn[s] for all reachable successors s.
            bitset_clear(&r.LiveOut[b]);
            for (int k = 0; k < 2; k++) {
                int s = blocks[b].succ[k];
                if (s >= 0 && s < nBlocks && !(reachable && !reachable[s]))
                    bitset_or(&r.LiveOut[b], &r.LiveIn[s]);
            }

            // LiveIn[b] = Use[b] ∪ (LiveOut[b] − Def[b])
            // Compute into tmp first to detect whether anything changed.
            bitset_diff(&tmp, &r.LiveOut[b], &r.Def[b]); // tmp = LiveOut[b] \ Def[b]
            bitset_union_into(&tmp, &tmp, &r.Use[b]);     // tmp |= Use[b]

            if (!bitset_equal(&r.LiveIn[b], &tmp)) {
                bitset_copy(&r.LiveIn[b], &tmp);
                changed = 1; // LiveIn grew — need another iteration
            }
        }
    }

    return r;
}

/* =========================================================================
 * Per-instruction liveness
 * =========================================================================
 * A single backward sweep (no iteration) over all blocks, seeded with the
 * fixed-point LiveOut[b] from liveness_computeCore.
 *
 * Transfer function (backward, one instruction at a time):
 *   liveAfter[i]       = live set at program point just after instruction i
 *   live_before(i)     = (liveAfter[i] − def(i)) ∪ use(i)
 * liveAfter[i] is recorded before the transfer function is applied so it
 * reflects the live set at the point just after i commits its result.
 * ========================================================================= */

LiveSet *liveness_computePerInstr(int nBlocks, const BasicBlock *blocks,
                                     int instrCount, int numVars,
                                     const LiveSet *blockLiveOut,
                                     LivenessExtractFn extract, void *ctx,
                                     Arena *arena) {
    int words = (numVars + (BITS_PER_WORD - 1)) / BITS_PER_WORD;

    // Allocate liveAfter[] headers and a contiguous bit-word slab.
    LiveSet  *liveAfter = arena_alloc(arena, (size_t)instrCount * sizeof(LiveSet));
    uint64_t *allBits   = arena_alloc(arena,
                              (size_t)instrCount * (size_t)words * sizeof(uint64_t));
    memset(allBits, 0, (size_t)instrCount * (size_t)words * sizeof(uint64_t));

    // Point each liveAfter[i].bits into its slice of the slab.
    for (int i = 0; i < instrCount; i++)
        liveAfter[i] = (LiveSet){ allBits + (size_t)i * (size_t)words, words };

    int uses[LIVENESS_MAX_IDS], defs[LIVENESS_MAX_IDS], nUses, nDefs;

    // Scratch live set reused for each block's backward sweep.
    uint64_t *liveBits = arena_alloc(arena, (size_t)words * sizeof(uint64_t));
    LiveSet   live     = { liveBits, words };

    for (int b = 0; b < nBlocks; b++) {
        // Seed the sweep with the block's fixed-point live-out.
        bitset_copy(&live, &blockLiveOut[b]);

        // Scan instructions in reverse order within the block.
        for (int i = blocks[b].range.end - 1; i >= blocks[b].range.start; i--) {
            // Record what is live just after instruction i runs.
            bitset_copy(&liveAfter[i], &live);

            extract(ctx, i, uses, &nUses, defs, &nDefs);

            // Backward transfer: kill definitions first, then add uses.
            // Order matters: "x = x + 1" — x must be in live before the kill.
            // But here we remove defs THEN add uses, which is correct because:
            //   live_before(i) = (live_after(i) − def(i)) ∪ use(i)
            // If the same variable appears in both defs and uses (RMW), adding
            // uses after the kill re-inserts it — correct: the variable is live
            // before the instruction (it is read).
            for (int k = 0; k < nDefs; k++) bitset_clr(&live, defs[k]);
            for (int k = 0; k < nUses; k++) bitset_set(&live, uses[k]);
        }
    }
    return liveAfter;
}

/* =========================================================================
 * IR front-end
 * =========================================================================
 * Extracts uses and defs from IRInstr operands via VarMap, then calls the
 * generic engine.  Per-instruction liveAfter is not computed here because
 * IR-level passes (DCE, LICM, SR) only need block-granularity liveness.
 * ========================================================================= */

/** Context passed from liveness_computeIr to irExtract. */
typedef struct {
    IRFunction *f;
    VarMap     *varMap;
} IRLivenessCtx;

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
static void irExtract(void *ctxP, int instrIdx,
                       int uses[LIVENESS_MAX_IDS], int *nUses,
                       int defs[LIVENESS_MAX_IDS], int *nDefs) {
    IRLivenessCtx *ctx = ctxP;
    IRInstr *in = &ctx->f->instrs[instrIdx];
    *nUses = 0; *nDefs = 0;

    // src1 and src2 are always source operands — add them as uses.
    if (ir_operand_is_storage(in->src1.kind)) {
        int id = varmap_operand_id(ctx->varMap, in->src1);
        if (id >= 0) uses[(*nUses)++] = id;
    }
    if (ir_operand_is_storage(in->src2.kind)) {
        int id = varmap_operand_id(ctx->varMap, in->src2);
        if (id >= 0) uses[(*nUses)++] = id;
    }

    // Special case: IR_STORE_ARR  dst[src1] = src2
    // dst is the array base — it is READ to compute the effective address,
    // not written.  ir_defines_dst(IR_STORE_ARR) == false, so the branch
    // below correctly excludes it from defs, but we must add it as a use here.
    if (!ir_defines_dst(in->op) && ir_operand_is_storage(in->dst.kind)) {
        int id = varmap_operand_id(ctx->varMap, in->dst);
        if (id >= 0) uses[(*nUses)++] = id;
    }

    // For all opcodes that write a result into dst, dst is a def.
    if (ir_defines_dst(in->op) && ir_operand_is_storage(in->dst.kind)) {
        int id = varmap_operand_id(ctx->varMap, in->dst);
        if (id >= 0) defs[(*nDefs)++] = id;
    }
}

LivenessResult liveness_computeIr(IRFunction *f, const char *reachable,
                                    Arena *arena) {
    LivenessResult r = {0};

    // Pre-scan all instructions to assign a compact id to every distinct
    // operand (OPND_VAR or OPND_TEMP).  get-or-create semantics: same operand
    // encountered multiple times always yields the same id.
    r.varMap = varmap_init();
    for (int i = 0; i < f->count; i++) {
        IRInstr *in = &f->instrs[i];
        varmap_operand_id(&r.varMap, in->dst);
        varmap_operand_id(&r.varMap, in->src1);
        varmap_operand_id(&r.varMap, in->src2);
    }
    int numVars = r.varMap.nextId; // total distinct operands seen

    // Convert IRBlock descriptors to the generic BasicBlock layout expected
    // by liveness_computeCore (strips the IR-specific predCount field).
    BasicBlock *lb = arena_alloc(arena, (size_t)f->blockCount * sizeof(BasicBlock));
    for (int b = 0; b < f->blockCount; b++)
        lb[b] = f->blocks[b].bb;

    IRLivenessCtx ctx = { f, &r.varMap };
    r.blockSets = liveness_computeCore(f->blockCount, lb, numVars, reachable,
                                         irExtract, &ctx, arena);
    r.liveAfter = NULL; // IR-level passes do not need per-instruction granularity
    return r;
}

/* =========================================================================
 * Machine-code front-end
 * =========================================================================
 * Variable universe: vregs [0, nextVreg) and physical registers
 * [nextVreg, nextVreg + PHYS_ALLOCATABLE).  Physical registers participate
 * in liveness so that the interference graph correctly models ABI constraints
 * (e.g. a vreg live across a CALL must not be assigned a caller-saved phys reg).
 *
 * Unlike the IR front-end, per-instruction liveAfter[] is also computed here
 * because ig_build() needs it to add interference edges between each definition
 * and every variable live immediately after that definition point.
 * ========================================================================= */

/** Context passed from liveness_computeMach to machExtract. */
typedef struct {
    const MachFunction *f;
    int                 nextVreg; // base offset: phys reg P → id nextVreg + P
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

    // Collect implicit ABI defs (e.g. RAX/RDX clobbered by IDIV, caller-saved
    // regs clobbered by CALL).
    instr_implicit_defs(in, ctx->nextVreg, tmp, &n);
    for (int i = 0; i < n; i++) defs[(*nDefs)++] = tmp[i];
}

LivenessResult liveness_computeMach(const MachFunction *f,
                                      const BasicBlock *blocks,
                                      int nBlocks, Arena *arena) {
    LivenessResult r = {0};

    // Variable universe size: all vregs plus all allocatable physical registers.
    // Physical reg P is represented as id (nextVreg + P) so that both classes
    // share one contiguous id space and the same bit-set operations apply.
    MachLivenessCtx ctx = { f, f->nextVreg };
    int numVars = f->nextVreg + PHYS_ALLOCATABLE;

    // Run the backward dataflow engine (reachable = NULL → all blocks active).
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

