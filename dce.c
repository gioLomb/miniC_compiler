/**
 * @file dce.c
 * @brief Dead Code Elimination (DCE) pass on the linear IR.
 *
 * Overview
 * --------
 * DCE removes IR instructions whose results are provably never used, and
 * discards every instruction inside unreachable basic blocks.  It is a
 * cleanup pass: it does not discover optimisation opportunities on its own
 * but relies on upstream passes (SVN, CP, LICM, SR) to first introduce dead
 * code, which DCE then removes in O(n) time.
 *
 * Algorithm — four sequential phases
 * ------------------------------------
 *  1. Reachability  (mark_reachable_blocks)
 *     Depth-first search from the entry block (index 0) over CFG successor
 *     edges to identify unreachable blocks.  All instructions inside an
 *     unreachable block are unconditionally marked for elimination; no
 *     liveness information is needed for them.
 *
 *  2. Liveness  (liveness_computeIr)
 *     Backward dataflow analysis producing block-level LiveIn/LiveOut sets.
 *     A VarMap assigns compact integer ids to all Operands so that liveness
 *     can be stored as bit vectors and updated with bitwise operations.
 *
 *  3. Mark  (dce_mark)
 *     A single backward scan per reachable block, starting with LiveOut as
 *     the initial live set.  An instruction is marked dead when it is pure,
 *     defines a storage operand, and that operand is absent from the current
 *     live set (i.e., dead at this point).  The live set is updated after
 *     each instruction: sources are added, the destination is killed.
 *
 *  4. Sweep  (dce_sweep)
 *     The flat instruction array is compacted in a single forward pass,
 *     discarding eliminated entries.  Block start/end indices are rewritten
 *     to reflect each block's position in the new layout.
 *
 * Purity
 * ------
 * An instruction is pure if it computes a value solely from its operands,
 * without modifying memory, performing I/O, or affecting control flow:
 *
 *   Pure  : ADD, SUB, MUL, DIV, MOD, NEG, NOT, LT, LE, GT, GE, EQ, NE,
 *            ASSIGN, LOAD_ARR
 *   Impure: STORE_ARR, CALL, PARAM, RETURN, GOTO, IF_FALSE, LABEL
 *
 * LOAD_ARR is treated as pure (no alias analysis is performed); a store to
 * the same location would appear as the impure STORE_ARR and would never be
 * eliminated, preserving correctness.
 *
 * Interaction with the optimisation pipeline
 * ------------------------------------------
 * DCE is chained with CP in a fixed-point loop inside ir_buildFunction()
 * (ir.c).  Constant propagation may introduce dead assignments (a variable
 * replaced by a literal everywhere it is used); DCE removes them.  Removing
 * dead definitions may, in turn, make their sources dead, enabling another
 * CP + DCE round.  A second CP + DCE loop runs after LICM + SR to clean up
 * dead multiplications that SR replaces with cheaper additions.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "dce.h"
#include "liveness.h"
#include "arena.h"

/* =========================================================================
 * Purity predicate
 * =========================================================================
 * Only pure instructions with a dead destination may be eliminated.  Impure
 * instructions are always kept regardless of whether their result is live,
 * because removing them would alter observable program behaviour (memory
 * writes, calls, control flow).
 * ========================================================================= */

/**
 * @brief Return non-zero if opcode @p op has no observable side effects.
 *
 * Uses a precomputed bitmask for an O(1) test, avoiding a branch-heavy
 * switch that could inhibit inlining.  The mask covers every opcode that
 * computes a value without touching memory or affecting control flow.
 *
 * @param op  IR opcode to test.
 * @return    1 if @p op is a pure computation, 0 if it has side effects.
 */
static inline int is_pure(IROp op) {
    switch (op) {
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_NEG: case IR_NOT:
    case IR_LT:  case IR_LE:  case IR_GT:  case IR_GE:  case IR_EQ: case IR_NE:
    case IR_ASSIGN:
    case IR_LOAD_ARR: // treated as pure: no alias analysis, but STORE_ARR is impure
        return 1;    //   so a store to the same slot is never eliminated anyway
    default: return 0;
    }
}

/* =========================================================================
 * Phase 1 — Reachability analysis
 * =========================================================================
 * A depth-first search from the entry block (index 0) marks every block that
 * can be reached via CFG successor edges.  Blocks left unmarked can never
 * execute; all their instructions are dead by definition.
 *
 * Unreachable blocks arise naturally after CP folds a conditional branch into
 * an unconditional one, leaving one target with no live predecessors, or
 * after SR / LICM restructure the CFG during loop transformations.
 * ========================================================================= */

/**
 * @brief Mark every basic block reachable from the entry block (index 0).
 *
 * Uses an explicit stack-based DFS to avoid call-stack overflow on functions
 * with deeply nested control flow.  @p reachable must be zero-initialised by
 * the caller; on return, reachable[b] is 1 for every block reachable from
 * block 0 via succ[] edges.
 *
 * @param f          IR function whose CFG is traversed.
 * @param reachable  Output array of length @c f->blockCount (caller-zeroed).
 */
static void mark_reachable_blocks(IRFunction *f, char *reachable, Arena *arena) {
    if (f->blockCount == 0) return;

    // worst case: every block on the stack once → blockCount entries suffice
    int *stack = arena_alloc(arena, (size_t)f->blockCount * sizeof(int));
    int top = 0;

    // seed the DFS from the function entry block
    stack[top++] = 0;
    reachable[0] = 1; // mark before push so we never push the same block twice

    while (top > 0) {
        int b = stack[--top];
        for (int k = 0; k < 2; k++) { // each block has at most 2 successors (succ[0], succ[1])
            int s = f->blocks[b].bb.succ[k];
            // bounds check guards against sentinel -1 and stale succ values
            if (s >= 0 && s < f->blockCount && !reachable[s]) {
                reachable[s] = 1; // mark before push: prevents re-enqueuing in cyclic CFGs
                stack[top++] = s;
            }
        }
    }
}

/* =========================================================================
 * Phase 3 — Mark: backward liveness scan
 * =========================================================================
 * One backward pass per block over the flat instruction array.  The live set
 * is seeded with the block-level LiveOut computed by liveness_computeIr()
 * and updated instruction by instruction using the standard backward
 * dataflow transfer function:
 *
 *   live_before(i) = (live_after(i) - def(i)) ∪ use(i)
 *
 * An instruction qualifies for elimination when its opcode is pure, it
 * defines a storage operand (variable or temporary), and that operand's id
 * is absent from the current live set — meaning no successor instruction on
 * any execution path reads the value before it is overwritten or discarded.
 * ========================================================================= */

/**
 * @brief Populate @p eliminate[] by scanning each block backward.
 *
 * For every instruction @c i in every reachable block:
 *   - Unreachable blocks: all instructions are flagged unconditionally.
 *   - Reachable blocks: an instruction is flagged only when it is pure,
 *     defines a storage operand, and that operand is not live at this point.
 *     Otherwise the live set is updated: source operands are marked live,
 *     the destination operand is killed.
 *
 * A per-block BitSet is allocated from @p arena (one allocation per block)
 * and is reclaimed when the arena is destroyed by the caller.
 *
 * @param f          IR function to analyse.
 * @param reachable  Per-block reachability flags (from mark_reachable_blocks).
 * @param liv        Liveness result from liveness_computeIr(); provides
 *                   LiveOut per block and the VarMap for operand id lookup.
 * @param arena      Scratch arena for the per-block live set allocation.
 * @param eliminate  Output byte array of length @c f->count (caller-zeroed);
 *                   entries set to 1 identify instructions to be discarded.
 */
static void dce_mark(IRFunction *f, const char *reachable, LivenessResult *liv,
                      Arena *arena, char *eliminate) {
    int nBlocks = f->blockCount;
    int words   = liv->blockSets.words;
    int nInstrs = f->count;

    memset(eliminate, 0, (size_t)nInstrs);

    for (int b = 0; b < nBlocks; b++) {

        // unreachable block: all instructions are dead by definition
        if (!reachable[b]) {
            for (int i = f->blocks[b].bb.start; i < f->blocks[b].bb.end; i++)
                eliminate[i] = 1;
            continue;
        }

        // seed the backward scan with the block's live-out set
        BitSet live = bitset_new(arena, words);
        bitset_copy(&live, &liv->blockSets.LiveOut[b]);

        for (int i = f->blocks[b].bb.end - 1; i >= f->blocks[b].bb.start; i--) {
            IRInstr *in = &f->instrs[i];

            // two-part check: opcode must write a dst AND dst must be a trackable
            // storage location (OPND_VAR or OPND_TEMP); labels and function names
            // are not tracked by the VarMap and must not be treated as defs
            int def   = ir_DefinesDst(in->op) && ir_OperandIsStorage(in->dst.kind);
            int dstId = def ? varmap_operand_id(&liv->varMap, in->dst) : -1; // avoid lookup when not a def

            // pure instruction whose destination is dead at this point: eliminate
            if (is_pure(in->op) && def && dstId >= 0 && !bitset_test(&live, dstId)) {
                eliminate[i] = 1;
                // do NOT update the live set: sources of a dead instruction are
                // themselves potentially dead and must not be kept alive artificially
                continue;
            }

            // update live set: make sources live, kill the destination.
            // order matters — sources are added BEFORE the destination is killed.
            // this correctly handles self-referential patterns like "x = x + 1":
            // if we killed dst first, src (same variable) would look dead and its
            // liveness would not propagate backwards past this instruction
            if (ir_OperandIsStorage(in->src1.kind)) {
                int id = varmap_operand_id(&liv->varMap, in->src1);
                if (id >= 0) bitset_set(&live, id);
            }
            if (ir_OperandIsStorage(in->src2.kind)) {
                int id = varmap_operand_id(&liv->varMap, in->src2);
                if (id >= 0) bitset_set(&live, id);
            }
            if (def && dstId >= 0)
                bitset_clr(&live, dstId); // dst is defined here, so not live above
        }
    }
}

/* =========================================================================
 * Phase 4 — Sweep: compact the instruction array
 * =========================================================================
 * A single forward pass discards every entry flagged in eliminate[], writing
 * survivors into a fresh heap allocation.  Block start/end indices are
 * updated to reflect each block's new position in the compacted array.
 *
 * Note: this function is structurally identical to cp_sweep() in cp.c.
 * Factoring it into a shared helper (e.g. ir_compact_instrs() in ir.c)
 * would eliminate the duplication; left as-is to keep both passes
 * self-contained and avoid coupling their internal layouts.
 * ========================================================================= */

/**
 * @brief Compact @c f->instrs, discarding every instruction flagged in @p eliminate.
 *
 * Allocates a fresh instruction array, copies surviving instructions into it,
 * and replaces @c f->instrs with the new allocation (freeing the old one).
 * Updates @c f->blocks[b].bb.start and @c .end for every block and resets
 * @c f->curBlockStart to 0, since the compacted layout always starts at 0.
 *
 * @param f          IR function to compact (modified in place).
 * @param eliminate  Byte flag array of length @c f->count; 1 → discard.
 * @param nBlocks    Number of blocks (must equal @c f->blockCount).
 * @return           1 if at least one instruction was removed, 0 otherwise.
 */
static int dce_sweep(IRFunction *f, char *eliminate, int nBlocks) {
    int nInstrs = f->count;
    // upper bound: at most nInstrs survivors, so no realloc needed
    IRInstr *newInstrs = malloc((size_t)nInstrs * sizeof(IRInstr));
    int newCount = 0;

    for (int b = 0; b < nBlocks; b++) {
        int oldStart = f->blocks[b].bb.start;
        int oldEnd   = f->blocks[b].bb.end;
        int newStart = newCount; // snapshot position before copying this block's survivors

        for (int i = oldStart; i < oldEnd; i++) {
            if (!eliminate[i])
                newInstrs[newCount++] = f->instrs[i];
        }

        // rewrite indices to reflect the compacted layout
        f->blocks[b].bb.start = newStart;
        f->blocks[b].bb.end   = newCount; // newCount is now one past the last survivor
    }

    free(f->instrs);
    f->instrs        = newInstrs;
    f->count         = newCount;
    f->capacity      = newCount; // allocation is exactly sized; no slack needed
    f->curBlockStart = 0;        // reset: meaningful only during IR construction

    // changed iff the total count shrank — no extra flag variable needed
    return newCount != nInstrs;
}

/* =========================================================================
 * Public entry point
 * ========================================================================= */

/**
 * @brief Run one DCE iteration over @p f, returning whether anything changed.
 *
 * Orchestrates the four phases in order:
 *   1. Reachability  — identify blocks that can never execute.
 *   2. Liveness      — backward dataflow to determine live variables.
 *   3. Mark          — flag pure instructions with dead destinations.
 *   4. Sweep         — compact the instruction array, update block indices.
 *
 * All liveness data is allocated in a local arena that is destroyed on
 * return.  The VarMap embedded in the LivenessResult owns a hash table and
 * must be destroyed explicitly before the arena, as the hash table's backing
 * memory was allocated with malloc (not in the arena).
 *
 * @pre  @p f must have a valid, fully resolved CFG (succ[] filled for all blocks).
 * @post Unreachable instructions and dead pure instructions have been removed.
 *       All block start/end indices are consistent with the new layout.
 *
 * @param f  IR function to optimise (modified in place).
 * @return   1 if at least one instruction was eliminated, 0 if IR is unchanged.
 */
int dce_optimize(IRFunction *f) {
    if (!f || f->blockCount == 0 || f->count == 0) return 0;

    int nBlocks = f->blockCount;
    Arena *arena = arena_create(0);

    // phase 1: identify unreachable blocks
    char *reachable = arena_alloc(arena, (size_t)nBlocks);
    memset(reachable, 0, (size_t)nBlocks);
    mark_reachable_blocks(f, reachable, arena);

    // passing reachable to liveness_computeIr lets the dataflow engine skip
    // unreachable blocks entirely — both a performance win and a correctness
    // guard (unreachable blocks may have malformed or stale succ[] entries)

    // phase 2: backward liveness dataflow over the IR
    LivenessResult liv = liveness_computeIr(f, reachable, arena);

    // allocate eliminate AFTER liveness: f->count is stable at this point
    // (liveness_computeIr is read-only with respect to the instruction array)

    // phase 3: mark dead instructions
    char *eliminate = arena_alloc(arena, (size_t)f->count);
    dce_mark(f, reachable, &liv, arena, eliminate);

    // phase 4: compact the instruction array
    int changed = dce_sweep(f, eliminate, nBlocks);

    // varmap_destroy MUST come before arena_destroy: the VarMap's hash table
    // pool was allocated with malloc (not inside the arena), so the arena
    // cannot free it — only varmap_destroy() can
    varmap_destroy(&liv.varMap);
    arena_destroy(arena);

    return changed;
}
