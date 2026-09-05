/**
 * @file licm.c
 * @brief Loop-Invariant Code Motion (LICM) on the linear IR.
 *
 * Overview
 * --------
 * LICM hoists instructions whose operands do not change across loop
 * iterations out of the loop body and into a synthetic pre-header block
 * that runs exactly once before the loop begins.  This turns O(n) work
 * (repeated every iteration) into O(1) work (computed once).
 *
 * Algorithm — four sequential phases per loop
 * --------------------------------------------
 *  1. Def-count  (count_defs_in_loop)
 *     Count how many times each variable/temporary is defined inside the
 *     loop body.  An operand defined zero times is loop-invariant by
 *     definition (its value comes from outside the loop).  An operand
 *     defined exactly once is a *candidate* for invariance — it may still
 *     depend on other operands that are not invariant.  An operand defined
 *     two or more times can never be invariant.
 *
 *  2. Invariant detection  (find_invariants)
 *     Worklist algorithm: seed with all pure instructions whose operands
 *     are trivially invariant (constants or operands with defCount == 0),
 *     then propagate transitively — if an instruction is marked invariant,
 *     every instruction that uses its destination is re-evaluated.
 *     An instruction is invariant iff it is pure AND all its source
 *     operands are invariant (constant, defined outside, or their single
 *     in-loop definition is itself invariant).
 *
 *  3. Safety check + motion  (move_invariants)
 *     Not every invariant instruction is safe to hoist.  Two conditions
 *     must hold:
 *       a. The block containing the instruction must dominate ALL loop
 *          exits — otherwise hoisting could execute the instruction on
 *          a path where it would not have executed inside the loop.
 *       b. The destination must be defined exactly once in the loop AND
 *          must not be live-in at the loop header — otherwise hoisting
 *          would change the value seen by paths that enter the loop from
 *          outside without going through the pre-header.
 *     Safe instructions are moved into the pre-header in source order.
 *
 *  4. Pre-header insertion  (loop_build_pre_header, called before phase 3)
 *     A synthetic block is inserted between the loop's non-back-edge
 *     predecessors and the header.  LICM places hoisted instructions there.
 *     The block is created by loop.c and shared with SR.
 *
 * Purity
 * ------
 * Only pure instructions can be hoisted.  Impure instructions (stores,
 * calls, branches) have observable side effects that must not be
 * reordered relative to loop iterations:
 *
 *   Pure  : ADD, SUB, MUL, DIV, MOD, NEG, NOT, LT, LE, GT, GE, EQ, NE,
 *            ASSIGN
 *   Impure: STORE_ARR, LOAD_ARR, CALL, PARAM, RETURN, GOTO, IF_FALSE, LABEL
 *
 * Note: LOAD_ARR is treated as impure here, unlike in DCE.  A load from an
 * array whose base pointer is invariant could in principle be hoisted, but
 * without alias analysis we cannot rule out a STORE_ARR to the same slot
 * inside the loop body — so we conservatively keep all array accesses in place.
 *
 * Interaction with the optimisation pipeline
 * ------------------------------------------
 * licm_optimize() is called once per function after the initial SVN+DCE+CP
 * rounds.  It returns 1 if any instruction was moved, prompting a new
 * CP+DCE round to clean up the dead multiplications left behind by SR
 * and any constants newly exposed by hoisting.
 *
 * Loop detection and dominator computation are delegated to loop.c.
 * Liveness is computed by liveness.c (IR front-end) and is used to check
 * the live-in condition at the loop header.
 */

#include <stdlib.h>
#include <string.h>
#include "licm.h"
#include "loop.h"
#include "liveness.h"
#include "arena.h"
#include "dynamic_array.h"


/* =========================================================================
 * Phase 1 — Def-count inside the loop
 * =========================================================================
 * Walk every instruction in every block of the loop body and count how
 * many times each tracked operand (variable or temporary) is defined.
 * The counts guide invariant detection:
 *   defCount == 0  → defined outside the loop, trivially invariant
 *   defCount == 1  → single definition, may be invariant (checked in phase 2)
 *   defCount >= 2  → redefined, can never be invariant
 * ========================================================================= */

/**
 * @brief Count definitions of every tracked operand inside loop @p L.
 *
 * Allocates and returns a zero-initialised array of length @p numVars
 * from @p arena.  Only instructions that define a storage operand
 * (OPND_VAR or OPND_TEMP) are counted; constants and labels are ignored.
 *
 * @param f       IR function containing the loop.
 * @param L       Loop whose body blocks are scanned.
 * @param vm      VarMap for operand-to-id translation.
 * @param numVars Total number of tracked variable ids.
 * @param arena   Arena for the output array allocation.
 * @return        Array defCount[0..numVars-1]; caller must not free it.
 */
static int *count_defs_in_loop(IRFunction *f, Loop *L, VarMap *vm,
                               int numVars, Arena *arena) {
    int *defCount = arena_alloc(arena, (size_t)numVars * sizeof(int));
    memset(defCount, 0, (size_t)numVars * sizeof(int));

    const int bodyCount = L->bodyCount;
    const IRBlock *blocks = f->blocks;
    const IRInstr *instrs = f->instrs;

    for (int i = 0; i < bodyCount; i++) {
        int b = L->body[i];
        int start = blocks[b].bb.range.start;
        int end   = blocks[b].bb.range.end;

        for (int j = start; j < end; j++) {
            const IRInstr *in = &instrs[j];
            
            // Short-circuiting combinato per le guardie dell'istruzione
            if (ir_defines_dst(in->op) && ir_operand_is_storage(in->dst.kind)) {
                int id = varmap_operand_id(vm, in->dst);
                if (id >= 0) {
                    defCount[id]++;
                }
            }
        }
    }

    return defCount;
}


// static inline int operand_identity_mismatch(Operand dst, Operand op) {
// //     if (op.kind == OPND_VAR)
// //         return dst.data.varLevel  != op.data.varLevel ||
// //                dst.data.varOffset != op.data.varOffset;
// //     if (op.kind == OPND_TEMP)
// //         return dst.data.tempId != op.data.tempId;
// //     return 0; // unknown kind: conservatively no mismatch
// // }

/**
 * @brief Scan a single block's instruction range for a definition of @p op,
 *        updating *foundIdx with the (last) matching definition's index.
 *
 * @param f          Function whose instrs[] is scanned.
 * @param b          Block index to scan.
 * @param op         Operand whose definition(s) we're looking for.
 * @param invariant  Per-instruction "is this def loop-invariant" flags.
 * @param foundIdx   In/out: updated to j for every matching definition found
 *                   in this block (last match wins, consistent with the
 *                   original single loop's behaviour).
 * @return 0 to keep scanning the remaining blocks, or -1 if a matching
 *         definition was found that is NOT marked invariant — the caller
 *         must abort the whole search immediately in that case.
 */
static int scan_block_for_def(IRFunction *f, int b, Operand op,
                               const char *invariant, int *foundIdx) {
    const IRInstr *instrs = f->instrs; // cache pointer, evita reload da f ogni iter
    int start = f->blocks[b].bb.range.start;
    int end   = f->blocks[b].bb.range.end;

    for (int j = start; j < end; j++) {
        const IRInstr *in = &instrs[j];

        if (!ir_defines_dst(in->op) || in->dst.kind != op.kind) continue; // fuse cold guards

        if (!ir_is_same_operand(&(in->dst),&op)) continue;

        if (!invariant[j]) return -1;
        *foundIdx = j;
    }
    return 0;
}

/**
 * @brief If @p op is defined exactly once in @p L, return that instruction
 *        index; return -1 if @p op is not a storage operand, is not defined
 *        in the loop, or is defined more than once or by a non-invariant
 *        instruction.
 *
 * Used by src_is_invariant to check the "single definition and that definition
 * is itself invariant" case.
 *
 * @param f         IR function.
 * @param L         Loop being analysed.
 * @param op        Operand to locate.
 * @param invariant Per-instruction invariance flags (phase 2 output so far).
 * @return          Index of the unique defining instruction, or -1.
 */
static int single_loop_def_invariant(IRFunction *f, Loop *L, Operand op,
                                   const char *invariant) {
    if (!ir_operand_is_storage(op.kind)) return -1;

    int found = -1;
    for (int i = 0; i < L->bodyCount; i++) {
        int b = L->body[i];
        if (scan_block_for_def(f, b, op, invariant, &found) < 0) return -1;
    }

    return found;
}

/**
 * @brief Return non-zero if source operand @p src is loop-invariant.
 *
 * An operand is invariant if any of the following holds:
 *   - it is a constant or absent (OPND_CONST_INT, OPND_CONST_FLOAT, OPND_NONE)
 *   - it is not a storage operand (label, function name)
 *   - it has no definition inside the loop (defCount == 0)
 *   - it has exactly one definition in the loop and that definition is
 *     already marked invariant (transitive invariance)
 *
 * @param f         IR function.
 * @param L         Loop being analysed.
 * @param src       Source operand to test.
 * @param defCount  Per-id definition counts from count_defs_in_loop.
 * @param vm        VarMap for id resolution.
 * @param invariant Per-instruction invariance flags built so far.
 * @return          1 if @p src is invariant, 0 otherwise.
 */
static int src_is_invariant(IRFunction *f, Loop *L, Operand src,
                           const int *defCount, VarMap *vm, const char *invariant) {
   if (src.kind == OPND_CONST_INT || src.kind == OPND_CONST_FLOAT ||
        src.kind == OPND_NONE      || src.kind == OPND_GLOBAL) return 1;
 
    if (!ir_operand_is_storage(src.kind)) return 0;

    int id = varmap_operand_id(vm, src);

    // defined outside the loop: value is fixed for all iterations
    if (id < 0 || defCount[id] == 0) return 1;

    // exactly one definition: invariant iff that definition is itself invariant
    if (defCount[id] == 1) return single_loop_def_invariant(f, L, src, invariant) >= 0;

    // two or more definitions: value changes across iterations → not invariant
    return 0;
}

/*
 * Invariant detection via worklist
 *
*/

/** Historical name preserved: UsedByList is now just an IntVector. */
typedef IntVector UsedByList;

/**
 * @brief Free the dynamic arrays of all @p numVars UsedByList entries.
 */
static void free_used_by(UsedByList *usedBy, int numVars) {
    for (int i = 0; i < numVars; i++)
        int_vector_free(&usedBy[i]);
}

/**
 * @brief Allocate and zero-initialize a usedBy[] array of length numVars.
 *
 * usedBy[id] = list of instruction indices (in the loop) that read operand id.
 */
static UsedByList *alloc_used_by(int numVars) {
    UsedByList *usedBy = calloc((size_t)numVars, sizeof(UsedByList));
    if (!usedBy) abort();
    for (int id = 0; id < numVars; id++) int_vector_init(&usedBy[id], 0);
    return usedBy;
}

/**
 * @brief Register instruction j as a user of both its source operands.
 *
 * For each source operand that resolves to a tracked var id, push
 * instruction index j into usedBy[id], so propagate_worklist can later
 * find and re-check this instruction once that id becomes invariant.
 *
 * @param vm      VarMap for id resolution.
 * @param usedBy  Reverse-use map to update.
 * @param in      Instruction being registered.
 * @param j       Index of in inside f->instrs.
 */
static void register_src_uses(VarMap *vm, UsedByList *usedBy, IRInstr *in, int j) {
    Operand srcs[2] = { in->src1, in->src2 };
    for (int s = 0; s < 2; s++) {
        int id = varmap_operand_id(vm, srcs[s]);
        if (id >= 0) int_vector_push(&usedBy[id], j);
    }
}

/**
 * @brief Scan the loop body for pure, storage-defining instructions:
 *        register each as a user of its source operands in usedBy[], and
 *        enqueue it into worklist as a seed candidate (its sources may
 *        already be invariant).
 *
 * @param f        IR function.
 * @param L        Loop being analysed.
 * @param vm       VarMap for id resolution.
 * @param usedBy   Reverse-use map to populate (must already be allocated).
 * @param worklist Output worklist, sized f->count by the caller.
 * @return Number of instructions seeded into worklist (the new wTail).
 */
static int seed_worklist(IRFunction *f, Loop *L, VarMap *vm,
                         UsedByList *usedBy, int *worklist) {
    int wTail = 0;

    const int bodyCount = L->bodyCount;
    const IRBlock *blocks = f->blocks;
    const IRInstr *instrs = f->instrs;

    for (int i = 0; i < bodyCount; i++) {
        int b = L->body[i];
        int start = blocks[b].bb.range.start;
        int end   = blocks[b].bb.range.end;

        for (int j = start; j < end; j++) {
            const IRInstr *in = &instrs[j];

            // Filter pure candidate instructions writing to a storage destination
            if (ir_is_pure(in->op) && ir_defines_dst(in->op) &&
                ir_operand_is_storage(in->dst.kind)) {

                // register this instruction as a user of its source operands
                register_src_uses(vm, usedBy, (IRInstr *)in, j);

                // immediately enqueue: sources may already be invariant
                worklist[wTail++] = j;
            }
        }
    }

    return wTail;
}
/**
 * @brief Push into worklist every not-yet-invariant instruction that uses
 *        dstId, so it gets re-checked now that dstId is invariant.
 *
 * No-op if dstId is negative (operand not tracked).
 *
 * @param usedBy    Reverse-use map.
 * @param dstId     Var id that just became invariant (-1 if none).
 * @param worklist  Worklist array, grown in place.
 * @param wTail     In/out cursor: current worklist length, advanced by
 *                  the number of dependents pushed.
 * @param invariant invariant[] flags, used to skip already-marked instrs.
 */
static void enqueue_dependents(const UsedByList *usedBy, int dstId,
                                int *worklist, int *wTail, const char *invariant) {
    if (dstId < 0) return;
    for (int k = 0; k < usedBy[dstId].len; k++) {
        int dep = usedBy[dstId].data[k];
        if (!invariant[dep])
            worklist[(*wTail)++] = dep;
    }
}

/**
 * @brief Propagate invariance to a fixed point: pop an instruction, check
 *        that both its sources are invariant, mark it invariant, and
 *        enqueue every instruction that uses its dst (they may now become
 *        invariant too).
 *
 * @param usedBy   Reverse-use map built by seed_worklist.
 * @param worklist Worklist array, sized f->count by the caller; grows in
 *                 place as new dependents are pushed.
 * @param wTail    Number of entries already seeded into worklist.
 * @param invariant Output array of length f->count; invariant[j] is set
 *                 to 1 for every loop-invariant instruction.
 */
static void propagate_worklist(IRFunction *f, Loop *L, VarMap *vm,
                                const int *defCount, UsedByList *usedBy,
                                int *worklist, int wTail, char *invariant) {
    int wHead = 0;

    while (wHead < wTail) {
        int j = worklist[wHead++];

        if (invariant[j]) continue; // cheap check first — already marked, skip expensive src checks

        const IRInstr *in = &f->instrs[j]; // cache only after cheap guard passes

        if (!src_is_invariant(f, L, in->src1, defCount, vm, invariant)) continue;
        if (!src_is_invariant(f, L, in->src2, defCount, vm, invariant)) continue;

        invariant[j] = 1;

        int dstId = varmap_operand_id(vm, in->dst);
        enqueue_dependents(usedBy, dstId, worklist, &wTail, invariant);
    }
}

/**
 * @brief Mark every loop-invariant instruction in @p invariant[].
 *
 * Builds the usedBy reverse map, seeds the worklist with instructions
 * whose sources are trivially invariant, then propagates transitively.
 * On return, invariant[j] == 1 for every instruction index j inside the
 * loop body that is loop-invariant.
 *
 * @param f         IR function.
 * @param L         Loop being analysed.
 * @param vm        VarMap for id resolution.
 * @param numVars   Total number of tracked operand ids.
 * @param defCount  Per-id definition counts from count_defs_in_loop.
 * @param invariant Output array of length f->count, zeroed by the caller.
 * @param arena     Arena used for the phase-local output arrays.
 */
static void find_invariants(IRFunction *f, Loop *L, VarMap *vm,
                            int numVars, const int *defCount,
                            char *invariant, Arena *arena) {
    int n = f->count;

    UsedByList *usedBy = alloc_used_by(numVars);

    /* worklist: dimensione massima nota, quindi arena */
    int *worklist = arena_alloc(arena, (size_t)n * sizeof(int));

    int wTail = seed_worklist(f, L, vm, usedBy, worklist);
    propagate_worklist(f, L, vm, defCount, usedBy, worklist, wTail, invariant);

    free_used_by(usedBy, numVars);
    free(usedBy);
}


/**
 * @brief Return non-zero if block @p blk dominates all exit blocks of @p L.
 *
 * If the block containing an invariant instruction does not dominate every
 * loop exit, hoisting would cause the instruction to execute on iterations
 * that would have exited before reaching it — a change in semantics
 * (and potentially a crash if the instruction would have trapped).
 *
 * @param L    Loop whose exit blocks are checked.
 * @param Dom  Dominator sets from loop_compute_dominators.
 * @param blk  Block index to test.
 * @return     1 if @p blk dominates every exit block of @p L.
 */
static inline int dominates_all_exits(Loop *L, LiveSet *Dom, int blk) {
    for (int i = 0; i < L->exitCount; i++)
        if (!loop_dominates(Dom, blk, L->exits[i])) return 0;
    return 1;
}



/* =========================================================================
 * Phase 3 — Safety check and motion
 * =========================================================================
 * Walk all invariant instructions, filter to the safe subset, and move
 * them into the pre-header in a single array rebuild.
 *
 * The rebuild works in three phases:
 *   a. Copy instructions before the loop header unchanged.
 *   b. Append the safe invariant instructions into the pre-header range.
 *   c. Copy the remaining loop instructions, skipping the moved ones,
 *      and update an oldToNew[] index map so block start/end can be
 *      rewritten consistently.
 * ========================================================================= */


/**
 * @brief Build a flat instruction-index -> block-index lookup table.
 *
 * Blocks partition f->instrs[] contiguously in increasing order (see
 * ir_close_block() in ir.c: every block's [start,end) is disjoint and blocks
 * appear in index order), so a single forward scan over blocks fills the
 * whole map in O(instrs + blocks) total. Replaces the previous pattern of
 * calling instr_block() (O(blocks) linear scan) once per instruction inside
 * mark_hoistable()'s loop over every instruction in the function — an
 * O(instrs * blocks) cost per loop processed by LICM.
 *
 * @param f      IR function whose blocks are scanned.
 * @param arena  Arena the output array is allocated from.
 * @return       Arena-allocated array of length f->count; instrToBlock[j] is
 *               the index of the block containing instruction j.
 */
static int *build_instr_to_block(IRFunction *f, Arena *arena) {
    int *instrToBlock = arena_alloc(arena, (size_t)f->count * sizeof(int));
    for (int b = 0; b < f->blockCount; b++)
        for (int j = f->blocks[b].bb.range.start; j < f->blocks[b].bb.range.end; j++)
            instrToBlock[j] = b;
    return instrToBlock;
}


static int mark_hoistable(IRFunction *f, Loop *L, LiveSet *Dom,
                           const char *invariant, const int *defCount,
                           VarMap *vm, LivenessResult *liv, int header,
                           const char *inBody, const int *instrToBlock, char *doMove) {
    int moved = 0;

    for (int j = 0; j < f->count; j++) {
        if (!invariant[j]) continue;

        int blk = instrToBlock[j];  // O(1) lookup instead of instr_block() scan
        if (!inBody[blk]) continue;

        // containing block must dominate all loop exits
        if (!dominates_all_exits(L, Dom, blk)) continue;

        // exactly one definition of the destination in the loop
        int dstId = varmap_operand_id(vm, f->instrs[j].dst);
        if (dstId < 0 || defCount[dstId] != 1) continue;

        // destination must not be live-in at the header
        if (bitset_test(&liv->blockSets.LiveIn[header], dstId)) continue;

        doMove[j] = 1;
        moved++;
    }

    return moved;
}


/** @brief Result of compacting instrs into [prefix | hoisted | rest] layout. */
typedef struct {
    IRInstr *instrs;       // newly allocated, compacted instruction array
    int      count;        // number of instructions in instrs
    int      preHeaderMovedStart; // first index of the hoisted block (new pre-header body)
    int      preHeaderMovedEnd;   // one-past-last index of the hoisted block
} HoistLayout;

/**
 * @brief Rebuild the instruction array with hoisted instructions moved
 *        right before the loop header, and fill oldToNew for remapping.
 *
 * Layout of the new array: [0, insertAt) unchanged prefix, then every
 * instruction with doMove[j] set (this becomes the pre-header body), then
 * every remaining instruction from [insertAt, nInstrs) in original order.
 *
 * @param f         IR function (source instrs, not yet modified).
 * @param doMove    doMove[j] = 1 if instruction j must be hoisted.
 * @param moved     Number of instructions with doMove[j] set (== count of 1s).
 * @param insertAt  First instruction index of the loop header block.
 * @param oldToNew  Output map, length f->count. Filled for EVERY j with
 *                  doMove[j] == 0 (both the unchanged prefix and the
 *                  surviving suffix); left untouched for hoisted j — callers
 *                  must gate access on doMove[j], not on a sentinel value.
 * @return Layout describing the new array and where the hoisted block sits.
 */ //todo: arena
static HoistLayout compact_and_hoist(IRFunction *f, const char *doMove, int moved,
                                      int insertAt, int *oldToNew) {
    int nInstrs = f->count;
    HoistLayout out;
    out.instrs = malloc((size_t)(nInstrs + moved) * sizeof(IRInstr));
    out.count  = 0;

    // --- part a: instructions before the loop header (unchanged position) ---
    // FIX: oldToNew must be filled here too (identity map), not left at -1,
    // otherwise remap_block_ranges mistakes "never written" for "hoisted".
    for (int j = 0; j < insertAt; j++) {
        oldToNew[j] = j;
        out.instrs[out.count++] = f->instrs[j];
    }

    //  hoisted instructions that will form the pre-header body 
    out.preHeaderMovedStart = out.count;
    for (int j = 0; j < nInstrs; j++) {
        if (doMove[j]) out.instrs[out.count++] = f->instrs[j];
    }
    out.preHeaderMovedEnd = out.count;

    // remaining loop instructions (skipping moved ones) ---
    for (int j = insertAt; j < nInstrs; j++) {
        if (doMove[j]) continue; // already placed in part b
        oldToNew[j] = out.count;
        out.instrs[out.count++] = f->instrs[j];
    }

    return out;
}

/**
 * @brief Rewrite block.start/end ranges to match the compacted instruction
 *        array produced by compact_and_hoist.
 *
 * The pre-header block is special-cased: it now contains exactly the
 * hoisted instructions ([preHeaderMovedStart, preHeaderMovedEnd)). Every other block's
 * range is derived by mapping its old instructions through oldToNew and
 * skipping the ones that were hoisted out of it; a block left with no
 * surviving instructions becomes the empty range {0, 0}.
 *
 * @param f            IR function whose blocks[] are rewritten in place.
 * @param oldToNew     Map from compact_and_hoist, valid for every j with
 *                      doMove[j] == 0.
 * @param doMove       doMove[j] = 1 if instruction j was hoisted.
 * @param phIdx        Index of the pre-header block.
 * @param preHeaderMovedStart First index of the hoisted block in the new array.
 * @param preHeaderMovedEnd   One-past-last index of the hoisted block.
 */
static void remap_block_ranges(IRFunction *f, const int *oldToNew, const char *doMove,
                                int phIdx, int preHeaderMovedStart, int preHeaderMovedEnd) {
    for (int b = 0; b < f->blockCount; b++) {
        if (b == phIdx) {
            // pre-header now contains exactly the hoisted instructions
            f->blocks[b].bb.range.start = preHeaderMovedStart;
            f->blocks[b].bb.range.end   = preHeaderMovedEnd;
            continue;
        }

        int oldS = f->blocks[b].bb.range.start, oldE = f->blocks[b].bb.range.end;
        int newS = -1, newE = -1;

        for (int j = oldS; j < oldE; j++) {
            // FIX: gate on doMove (ground truth), not on oldToNew's sentinel —
            // oldToNew is now fully populated for non-hoisted j anyway.
            if (doMove[j]) continue; // this instruction was hoisted out of b
            if (newS == -1) newS = oldToNew[j];
            newE = oldToNew[j] + 1;
        }

        f->blocks[b].bb.range.start = (newS == -1) ? 0 : newS;
        f->blocks[b].bb.range.end   = (newE == -1) ? 0 : newE;
    }
}

/**
 * @brief Move safe loop-invariant instructions from the loop body to the
 *        pre-header.
 *
 * Safety conditions (both required):
 *   1. The containing block dominates all loop exits (dominates_all_exits).
 *   2. The destination has exactly one definition in the loop AND is not
 *      live-in at the loop header (moving it would not create a new value
 *      visible on paths that bypass the pre-header).
 *
 * After motion, block start/end indices for all blocks are updated to
 * reflect the new layout.  f->curBlockStart is reset to 0.
 *
 * @param f         IR function (modified in place).
 * @param L         Loop whose invariants are to be hoisted.
 * @param Dom       Dominator sets.
 * @param invariant Per-instruction invariance flags from find_invariants.
 * @param defCount  Per-id definition counts from count_defs_in_loop.
 * @param vm        VarMap for id resolution.
 * @param liv       Liveness result used to query live-in at the header.
 * @return          Number of instructions actually moved (0 = nothing changed).
 */
static int move_invariants(IRFunction *f, Loop *L, LiveSet *Dom,
                           const char *invariant, const int *defCount,
                           VarMap *vm, LivenessResult *liv) {
    int nInstrs = f->count, nBlocks = f->blockCount;
    int header  = L->header, phIdx = L->preHeader;

    Arena *localArena = arena_create(0);

    char *doMove = arena_alloc(localArena, (size_t)nInstrs);
    char *inBody = arena_alloc(localArena, (size_t)nBlocks);
    memset(doMove, 0, (size_t)nInstrs);
    memset(inBody, 0, (size_t)nBlocks);

    for (int i = 0; i < L->bodyCount; i++) inBody[L->body[i]] = 1;

    // precomputed once per loop instead of per-instruction linear scan
    int *instrToBlock = build_instr_to_block(f, localArena);

    int moved = mark_hoistable(f, L, Dom, invariant, defCount, vm, liv,
                                header, inBody, instrToBlock, doMove);
    if (!moved) { arena_destroy(localArena); return 0; }

    // insertAt: first instruction index of the loop header block —
    // hoisted instructions are placed in the pre-header just before it
    int insertAt = f->blocks[header].bb.range.start;

    int *oldToNew = arena_alloc(localArena, (size_t)nInstrs * sizeof(int));

    HoistLayout layout = compact_and_hoist(f, doMove, moved, insertAt, oldToNew);

    free(f->instrs);
    f->instrs   = layout.instrs;
    f->count    = layout.count;
    f->capacity = layout.count;

    remap_block_ranges(f, oldToNew, doMove, phIdx, layout.preHeaderMovedStart, layout.preHeaderMovedEnd);

    f->curBlockStart = 0;
    arena_destroy(localArena);
    return moved;
}

/* =========================================================================
 * Public entry point
 * ========================================================================= */

int licm_optimize(IRFunction *f, Arena *arenaScratch) {
    if (!f || f->blockCount == 0 || f->count == 0) return 0;

    int nBlocks = f->blockCount;
    int words   = (nBlocks + 63) / 64;
    // Outer scratch (dominators, loop descriptors, invariant flags): caller-owned.
    arena_reset(arenaScratch);

    // compute dominators and find natural loops
    LiveSet *Dom    = loop_compute_dominators(f, words, arenaScratch);
    Loop    *loops  = arena_alloc(arenaScratch, MAX_LOOPS * sizeof(Loop));
    int      nLoops = loop_find(f, Dom, loops, arenaScratch);
    if (nLoops == 0) return 0;

    // compute initial liveness (needed for live-in check in move_invariants)
    // livArena stays internally managed: tied to the per-loop scan below and
    // recreated only when something is actually hoisted. Sharing arenaScratch
    // here would invalidate Dom/loops still in use by subsequent loop
    // iterations in this same call.
    Arena         *livArena = arena_create(0);
    LivenessResult  liv     = liveness_computeIr(f, NULL, livArena);
    int totalMoved = 0;

    for (int l = 0; l < nLoops; l++) {
        Loop *L = &loops[l];

        // insert the synthetic pre-header block before processing this loop
        loop_build_pre_header(f, L);

        VarMap *vm  = &liv.varMap;
        int numVars = liv.blockSets.numVars;

        // phase 1: count definitions inside the loop body
        int *defCount = count_defs_in_loop(f, L, vm, numVars, arenaScratch);

        // phase 2: find all loop-invariant instructions
        char *invariant = arena_alloc(arenaScratch, (size_t)f->count);
        memset(invariant, 0, (size_t)f->count);
        find_invariants(f, L, vm, numVars, defCount, invariant, arenaScratch);

        // phase 3: move safe invariants to the pre-header
        int moved = move_invariants(f, L, Dom, invariant, defCount, vm, &liv);
        totalMoved += moved;

        if (moved) {
            // liveness is stale after motion: recompute before the next loop
            varmap_destroy(&liv.varMap);
            arena_destroy(livArena);
            livArena = arena_create(0);
            liv = liveness_computeIr(f, NULL, livArena);
        }
    }

    varmap_destroy(&liv.varMap);
    arena_destroy(livArena);
    return totalMoved > 0;
}
