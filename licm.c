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

    for (int i = 0; i < L->bodyCount; i++) {
        int b = L->body[i];
        for (int j = f->blocks[b].bb.start; j < f->blocks[b].bb.end; j++) {
            IRInstr *in = &f->instrs[j];
            if (!ir_defines_dst(in->op) || !ir_operand_is_storage(in->dst.kind)) continue;
            int id = varmap_operand_id(vm, in->dst);
            if (id >= 0) defCount[id]++;
        }
    }

    return defCount;
}

/* =========================================================================
 * Phase 2 helpers — operand invariance queries
 * =========================================================================
 * Two predicates used by find_invariants to decide whether a source operand
 * is already known-invariant before the worklist processes the instruction
 * that defines it.
 * ========================================================================= */

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
        for (int j = f->blocks[b].bb.start; j < f->blocks[b].bb.end; j++) {
            IRInstr *in = &f->instrs[j];
            if (!ir_defines_dst(in->op)) continue;
            if (in->dst.kind != op.kind) continue;

            // match by kind-specific identity fields
            if (op.kind == OPND_VAR &&
                (in->dst.data.varLevel  != op.data.varLevel ||
                 in->dst.data.varOffset != op.data.varOffset)) continue;
            if (op.kind == OPND_TEMP && in->dst.data.tempId != op.data.tempId) continue;

            // definition found — it must itself be marked invariant
            if (!invariant[j]) return -1;
            found = j;
        }
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
    // constants and absent operands are trivially invariant
    if (src.kind == OPND_CONST_INT || src.kind == OPND_CONST_FLOAT ||
        src.kind == OPND_NONE) return 1;

    // non-storage operands (labels, funcs) are not live values — skip
    if (!ir_operand_is_storage(src.kind)) return 0;

    int id = varmap_operand_id(vm, src);

    // defined outside the loop: value is fixed for all iterations
    if (id < 0 || defCount[id] == 0) return 1;

    // exactly one definition: invariant iff that definition is itself invariant
    if (defCount[id] == 1) return single_loop_def_invariant(f, L, src, invariant) >= 0;

    // two or more definitions: value changes across iterations → not invariant
    return 0;
}

/* =========================================================================
 * Phase 2 — Invariant detection via worklist
 * =========================================================================
 * Propagates invariance transitively through data-flow chains.
 *
 * Initial seed: every pure instruction in the loop body whose sources are
 * immediately invariant (constants or operands with defCount == 0) is
 * added to the worklist.  We also build a reverse map usedBy[id] = list of
 * instruction indices that use the operand with that id, so that when an
 * instruction is marked invariant we can quickly find all instructions that
 * may now become invariant too.
 *
 * Propagation: when instruction j is popped from the worklist and its
 * sources are all invariant, it is marked invariant and its destination's
 * usedBy list is added to the worklist for re-evaluation.
 * ========================================================================= */

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

    // usedBy[id] = list of instruction indices (in the loop) that read operand id
    UsedByList *usedBy = calloc((size_t)numVars, sizeof(UsedByList));
    if (!usedBy) abort();
    for (int id = 0; id < numVars; id++)
        int_vector_init(&usedBy[id]);

    /* worklist: dimensione massima nota, quindi arena */
    int *worklist = arena_alloc(arena, (size_t)n * sizeof(int));
    int wHead = 0, wTail = 0;

    // seed: scan the loop body for pure instructions and build usedBy
    for (int i = 0; i < L->bodyCount; i++) {
        int b = L->body[i];
        for (int j = f->blocks[b].bb.start; j < f->blocks[b].bb.end; j++) {
            IRInstr *in = &f->instrs[j];

            // only pure instructions that write a storage dst are candidates
            if (!ir_is_pure(in->op) || !ir_defines_dst(in->op) ||
                !ir_operand_is_storage(in->dst.kind)) continue;

            // register this instruction as a user of its source operands
            Operand srcs[2] = { in->src1, in->src2 };
            for (int s = 0; s < 2; s++) {
                int id = varmap_operand_id(vm, srcs[s]);
                if (id >= 0) int_vector_push(&usedBy[id], j);
            }

            // immediately enqueue: sources may already be invariant
            worklist[wTail++] = j;
        }
    }

    // propagate: pop an instruction, check sources, mark and propagate if invariant
    while (wHead < wTail) {
        int j = worklist[wHead++];
        IRInstr *in = &f->instrs[j];

        // both sources must be invariant for the instruction to be invariant
        if (!src_is_invariant(f, L, in->src1, defCount, vm, invariant)) continue;
        if (!src_is_invariant(f, L, in->src2, defCount, vm, invariant)) continue;

        // already marked: nothing new to propagate
        if (invariant[j]) continue;

        invariant[j] = 1;

        // notify all instructions that use this instruction's destination —
        // they may now become invariant too
        int dstId = varmap_operand_id(vm, in->dst);
        if (dstId >= 0) {
            for (int k = 0; k < usedBy[dstId].len; k++) {
                int dep = usedBy[dstId].data[k];
                if (!invariant[dep])
                    worklist[wTail++] = dep;
            }
        }
    }

    free_used_by(usedBy, numVars);
    free(usedBy);
}

/* =========================================================================
 * Phase 3 helpers — safety predicates
 * =========================================================================
 * An invariant instruction is safe to hoist only if moving it cannot change
 * the program's observable behaviour.  Two conditions must be verified.
 * ========================================================================= */

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

/**
 * @brief Return the block index that contains instruction @p j.
 *
 * Linear search over blocks; acceptable because the number of blocks per
 * loop body is small in practice.
 *
 * @param f  IR function.
 * @param j  Instruction index.
 * @return   Block index, or -1 if not found (should not happen).
 */
static inline int instr_block(IRFunction *f, int j) {
    for (int b = 0; b < f->blockCount; b++)
        if (j >= f->blocks[b].bb.start && j < f->blocks[b].bb.end) return b;
    return -1;
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
    int moved   = 0;

    Arena *localArena = arena_create(0);

    // doMove[j] = 1 if instruction j passes all safety checks
    char *doMove = arena_alloc(localArena, (size_t)nInstrs);
    // inBody[b]  = 1 if block b is part of the loop body
    char *inBody = arena_alloc(localArena, (size_t)nBlocks);
    memset(doMove, 0, (size_t)nInstrs);
    memset(inBody, 0, (size_t)nBlocks);

    for (int i = 0; i < L->bodyCount; i++) inBody[L->body[i]] = 1;

    // determine which invariant instructions are safe to hoist
    for (int j = 0; j < nInstrs; j++) {
        if (!invariant[j]) continue;

        int blk = instr_block(f, j);
        if (!inBody[blk]) continue;

        // condition 1: containing block must dominate all loop exits
        if (!dominates_all_exits(L, Dom, blk)) continue;

        // condition 2a: exactly one definition of the destination in the loop
        int dstId = varmap_operand_id(vm, f->instrs[j].dst);
        if (dstId < 0 || defCount[dstId] != 1) continue;

        // condition 2b: destination must not be live-in at the header
        // (otherwise an external predecessor of the header uses the old value)
        if (bitset_test(&liv->blockSets.LiveIn[header], dstId)) continue;

        doMove[j] = 1;
        moved++;
    }

    if (!moved) { arena_destroy(localArena); return 0; }

    // insertAt: first instruction index of the loop header block —
    // hoisted instructions are placed in the pre-header just before it
    int insertAt = f->blocks[header].bb.start;

    IRInstr *newInstrs = malloc((size_t)(nInstrs + moved) * sizeof(IRInstr));
    int newCount = 0;

    // --- part a: instructions before the loop header (unchanged) ---
    for (int j = 0; j < insertAt; j++)
        newInstrs[newCount++] = f->instrs[j];

    // --- part b: hoisted instructions → they will form the pre-header body ---
    int phMovedStart = newCount;
    for (int j = 0; j < nInstrs; j++) {
        if (doMove[j]) newInstrs[newCount++] = f->instrs[j];
    }
    int phNewEnd = newCount;

    // --- part c: remaining loop instructions (skipping moved ones) ---
    // build oldToNew to remap block start/end indices after compaction
    int *oldToNew = arena_alloc(localArena, (size_t)nInstrs * sizeof(int));
    memset(oldToNew, -1, nInstrs * sizeof(oldToNew[0]));

    for (int j = insertAt; j < nInstrs; j++) {
        if (doMove[j]) continue; // already placed in part b
        oldToNew[j] = newCount;
        newInstrs[newCount++] = f->instrs[j];
    }

    free(f->instrs);
    f->instrs   = newInstrs;
    f->count    = newCount;
    f->capacity = newCount;

    // rewrite block start/end for every block
    for (int b = 0; b < nBlocks; b++) {
        if (b == phIdx) {
            // pre-header now contains exactly the hoisted instructions
            f->blocks[b].bb.start = phMovedStart;
            f->blocks[b].bb.end   = phNewEnd;
            continue;
        }

        int oldS = f->blocks[b].bb.start, oldE = f->blocks[b].bb.end;
        int newS = -1, newE = -1;

        for (int j = oldS; j < oldE; j++) {
            if (oldToNew[j] == -1) continue; // this instruction was hoisted
            if (newS == -1) newS = oldToNew[j];
            newE = oldToNew[j] + 1;
        }

        f->blocks[b].bb.start = (newS == -1) ? 0 : newS;
        f->blocks[b].bb.end   = (newE == -1) ? 0 : newE;
    }

    f->curBlockStart = 0;
    arena_destroy(localArena);
    return moved;
}

/* =========================================================================
 * Public entry point
 * ========================================================================= */

int licm_optimize(IRFunction *f) {
    if (!f || f->blockCount == 0 || f->count == 0) return 0;

    int nBlocks = f->blockCount;
    int words   = (nBlocks + 63) / 64;
    Arena *arena = arena_create(0);

    // compute dominators and find natural loops
    LiveSet *Dom    = loop_compute_dominators(f, words, arena);
    Loop    *loops  = arena_alloc(arena, MAX_LOOPS * sizeof(Loop));
    int      nLoops = loop_find(f, Dom, loops, arena);
    if (nLoops == 0) { arena_destroy(arena); return 0; }

    // compute initial liveness (needed for live-in check in move_invariants)
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
        int *defCount = count_defs_in_loop(f, L, vm, numVars, arena);

        // phase 2: find all loop-invariant instructions
        char *invariant = arena_alloc(arena, (size_t)f->count);
        memset(invariant, 0, (size_t)f->count);
        find_invariants(f, L, vm, numVars, defCount, invariant, arena);

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
    arena_destroy(arena);
    return totalMoved > 0;
}