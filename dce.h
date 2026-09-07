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
#include "varmap.h"

/**
 * @param f    IR function to optimise (modified in place).
 * @param vm   Shared operand->id VarMap, owned by the caller. Forwarded to
 *             liveness_computeIr() so the operand table is reused across
 *             the CP/DCE fixed-point loop instead of rebuilt every call.
 * @param arenaScratch  Scratch arena for reachability/liveness/elimination data.
 * @return 1 if at least one instruction was eliminated, 0 if IR is unchanged.
 */
int dce_optimize(IRFunction *f, VarMap *vm, Arena *arenaScratch);

#endif /* DCE_H */
