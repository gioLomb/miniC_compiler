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