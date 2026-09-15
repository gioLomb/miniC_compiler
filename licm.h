#ifndef LICM_H
#define LICM_H


/**
 * @file licm.h
 * @brief Loop-Invariant Code Motion (LICM) pass interface.
 *
 * Detects computations that are invariant with respect to a natural loop
 * and hoists them to a newly created pre-header block, reducing the amount
 * of work performed on every iteration of the loop.
 */

#include "ir.h"

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
int licm_optimize(IRFunction *f,Arena *arenaScratch);

#endif /* LICM_H */
