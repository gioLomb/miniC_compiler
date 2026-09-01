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
 *  4. Sweep  (ir_sweep)
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

/* Mark: backward liveness scan */
/**
 * @brief Mark @p op's bit as live if it is a storage operand.
 *
 * Constants, labels, and function names carry no VarMap id and are
 * silently skipped.
 */
static inline void mark_operand_live(BitSet *live, Operand op, VarMap *vm) {
    if (!ir_operand_is_storage(op.kind)) return;
    int id = varmap_operand_id(vm, op);
    if (id >= 0) bitset_set(live, id);
}

/**
 * @brief Mark every instruction in block @p b as dead (unreachable block).
 */
static inline void mark_block_dead(IRFunction *f, int b, char *eliminate) {
    for (int i = f->blocks[b].bb.range.start; i < f->blocks[b].bb.range.end; i++)
        eliminate[i] = 1;
}

/**
 * @brief Apply the backward-liveness transfer function to one instruction:
 *        decide whether it is dead, or advance @p live past it.
 *
 * @return 1 if @p in was marked dead in @p eliminate (live set left
 *         untouched — sources of dead code must not be kept alive
 *         artificially); 0 if @p in survives and @p live was updated.
 */
static int dce_process_instr(IRInstr *in, int idx, BitSet *live,
                              VarMap *vm, char *eliminate) {
    // opcode must write a dst AND dst must be a trackable storage location
    // (OPND_VAR/OPND_TEMP); labels and function names are never defs
    int def   = ir_defines_dst(in->op) && ir_operand_is_storage(in->dst.kind);
    int dstId = def ? varmap_operand_id(vm, in->dst) : -1; // skip lookup when not a def

    // pure instruction whose destination is dead here: eliminate
    if ((ir_is_pure(in->op) || in->op == IR_LOAD_ARR) && def && dstId >= 0 && !bitset_test(live, dstId)) {
        eliminate[idx] = 1;
        return 1;
    }

    /* order matters: sources added BEFORE dst is killed, so a self-
     * referential "x = x + 1" still sees x live going further back */
    mark_operand_live(live, in->src1, vm);
    mark_operand_live(live, in->src2, vm);

    // IR_STORE_ARR: dst is the array base, READ to form the address — a use
    if (!ir_defines_dst(in->op))
        mark_operand_live(live, in->dst, vm);

    if (def && dstId >= 0)
        bitset_clr(live, dstId);

    return 0;
}

static void dce_mark(IRFunction *f, const char *reachable, LivenessResult *liv,
                      Arena *arena, char *eliminate) {
    int nBlocks = f->blockCount;
    int words   = liv->blockSets.words;
    memset(eliminate, 0, (size_t)f->count);

    for (int b = 0; b < nBlocks; b++) {
        if (!reachable[b]) {
            mark_block_dead(f, b, eliminate);
            continue;
        }

        BitSet live = bitset_new(arena, words);
        bitset_copy(&live, &liv->blockSets.LiveOut[b]);

        for (int i = f->blocks[b].bb.range.end - 1; i >= f->blocks[b].bb.range.start; i--)
            dce_process_instr(&f->instrs[i], i, &live, &liv->varMap, eliminate);
    }
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
int dce_optimize(IRFunction *f,Arena *arenaScratch) {
    if (!f || f->blockCount == 0 || f->count == 0) return 0;

    int nBlocks = f->blockCount;
    arena_reset(arenaScratch);
    // identify unreachable blocks
    char *reachable = arena_alloc(arenaScratch, (size_t)nBlocks);
    memset(reachable, 0, (size_t)nBlocks);
    mark_reachable_blocks(f, reachable, arenaScratch);

    // passing reachable to liveness_computeIr lets the dataflow engine skip
    // unreachable blocks entirely — both a performance win and a correctness
    // guard (unreachable blocks may have malformed or stale succ[] entries)

    // backward liveness dataflow over the IR
    LivenessResult liv = liveness_computeIr(f, reachable, arenaScratch);

    // mark dead instructions
    char *eliminate = arena_alloc(arenaScratch, (size_t)f->count);
    dce_mark(f, reachable, &liv, arenaScratch, eliminate);

    // compact the instruction array
    int changed = ir_sweep(f, eliminate, nBlocks);

    // varmap_destroy MUST come before arena_destroy: the VarMap's hash table
    // pool was allocated with malloc (not inside the arena), so the arena
    // cannot free it — only varmap_destroy() can
    varmap_destroy(&liv.varMap);

    return changed;
}
