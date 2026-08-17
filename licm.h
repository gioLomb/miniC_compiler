#ifndef LICM_H
#define LICM_H

#include "ir.h"

/**
 * @file licm.h
 * @brief Loop-Invariant Code Motion (LICM) pass interface.
 *
 * LICM identifies instructions inside loop bodies whose operands do not
 * change across iterations (loop-invariant) and hoists them into a
 * synthetic pre-header block that executes exactly once before the loop.
 *
 * An instruction is loop-invariant iff:
 *   - it is pure (no memory writes, no control flow, no calls), AND
 *   - every source operand is either a compile-time constant, defined
 *     outside the loop, or defined exactly once inside the loop by an
 *     instruction that is itself loop-invariant (transitive invariance).
 *
 * A loop-invariant instruction is safe to hoist only when:
 *   - the block containing it dominates ALL loop exits (hoisting cannot
 *     cause the instruction to execute on a path where it would not have
 *     executed inside the loop), AND
 *   - its destination is defined exactly once in the loop AND is not
 *     live-in at the loop header (no external predecessor reads the old
 *     value before the loop begins).
 *
 * Prerequisites
 * -------------
 * - f must have a fully resolved CFG (succ[] set for all blocks).
 * - licm_optimize() internally builds dominators, detects loops, and
 *   computes liveness; callers do not need to provide these.
 * - licm_optimize() inserts synthetic pre-header blocks into f->blocks[].
 *   SR (sr.c) shares these pre-headers and must be run after LICM.
 *
 * Pipeline position
 * -----------------
 * Called once per function after the initial SVN → DCE → (CP+DCE)* rounds.
 * Returns 1 to signal that a new CP+DCE round should be run to clean up
 * dead multiplications and newly exposed constants.
 */

/**
 * @brief Run Loop-Invariant Code Motion on every natural loop of @p f.
 *
 * For each natural loop found (identified via back-edges in the CFG):
 *   1. Insert a synthetic pre-header block immediately before the header.
 *   2. Count how many times each operand is defined inside the loop body.
 *   3. Find all loop-invariant instructions via worklist propagation.
 *   4. Hoist the safe subset into the pre-header in source order.
 *   5. If anything was moved, recompute liveness before the next loop
 *      (the old liveness sets are stale after the pre-header gains new
 *      definitions).
 *
 * The function modifies @p f in place: instructions are moved, the
 * instruction array is rebuilt, and all block start/end indices are
 * updated to reflect the new layout.
 *
 * @pre  @p f has a valid, fully resolved CFG (succ[] filled for all blocks).
 * @post Loop-invariant instructions have been moved to pre-headers.
 *       All block start/end indices are consistent with the new layout.
 *       SR (sr.c) may share the inserted pre-header blocks.
 *
 * @param f  IR function to optimise (modified in place).
 * @return   1 if at least one instruction was moved, 0 if the IR is unchanged.
 */
int licm_optimize(IRFunction *f);

#endif /* LICM_H */