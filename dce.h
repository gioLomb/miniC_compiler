/**
 * @file dce.h
 * @brief Dead Code Elimination (DCE) pass interface.
 *
 * Exposes a single entry point, dce_optimize(), which removes IR instructions
 * whose computed values are provably unused along every execution path, and
 * unconditionally eliminates all instructions inside unreachable basic blocks.
 *
 * See dce.c for the full algorithm description.
 *
 * Pipeline position
 * -----------------
 * DCE is invoked at multiple points inside ir_buildFunction() (ir.c),
 * always paired with at least one other pass that may expose new dead code:
 *
 *   SVN → DCE → (CP + DCE)* → LICM + SR → (CP + DCE)*
 *
 * The (CP + DCE) loop runs to a fixed point: constant propagation may
 * materialise dead assignments, and eliminating those assignments may in
 * turn expose further propagation opportunities.  After LICM + SR, the
 * hoisted and strength-reduced instructions leave behind dead multiplications
 * in the loop body that a final CP + DCE round cleans up.
 *
 * Idempotence
 * -----------
 * Running dce_optimize() twice in a row on unchanged IR always returns 0 on
 * the second call — it is safe to call in a loop until no progress is made.
 */

#ifndef DCE_H
#define DCE_H

#include "ir.h"

/**
 * @brief Run one Dead Code Elimination iteration over @p f.
 *
 * Removes instructions that are either:
 *   - Inside an unreachable basic block (no path from the entry reaches it), or
 *   - Pure computations (no side effects) whose destination operand is dead
 *     at every point past the instruction (i.e., the value is never read).
 *
 * A pure instruction is one whose only effect is computing a value:
 * arithmetic, comparisons, logical operators, assignments, and array loads.
 * Impure instructions — stores, calls, returns, branches, labels — are
 * never eliminated regardless of whether their destination is live.
 *
 * @pre  @p f must have a valid, fully resolved CFG (succ[] set for all blocks).
 * @post Instructions in unreachable blocks and pure instructions with dead
 *       destinations have been removed.  All block start/end indices are
 *       updated to reflect the compacted layout.
 *
 * @param f  IR function to optimise (modified in place).
 * @return   1 if at least one instruction was removed, 0 if IR is unchanged.
 */
int dce_optimize(IRFunction *f);

#endif /* DCE_H */