#ifndef DCE_H
#define DCE_H

/**
 * @file dce.h
 * @brief Dead Code Elimination (DCE) pass interface for the linear IR.
 * Removes pure instructions whose results are never used and drops
 * every instruction inside unreachable basic blocks.
 */

#include "ir.h"
#include "varmap.h"

/**
 * @param f    IR function to optimise (modified in place).
 * @param vm   Shared operand-id VarMap, forwarded to liveness_computeIr()
 *             so its per-instruction id cache is reused instead of rebuilt.
 * @param arenaScratch  Scratch arena for reachability/liveness/elimination data.
 * @return 1 if at least one instruction was eliminated, 0 if IR is unchanged.
 */
int dce_optimize(IRFunction *f, VarMap *vm, Arena *arenaScratch);

#endif /* DCE_H */