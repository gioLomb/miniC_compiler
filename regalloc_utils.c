#include "regalloc_utils.h"

/* =========================================================================
 * Spill cost weighting
 * =========================================================================
 * Weight is 10^loopDepth (capped at depth 5 = 100 000) so that variables
 * live inside hot loops are strongly preferred for register allocation over
 * variables that are rarely executed. The register allocator sums these
 * weights across all def/use sites to build a per-vreg spill cost.
 * ========================================================================= */

int regalloc_spill_weight(int loopDepth) {
    static const int weights[] = {1, 10, 100, 1000, 10000, 100000};
    // clamp to the precomputed table range to avoid out-of-bounds access
    // and overflow for pathologically deep nesting
    if (loopDepth < 0) loopDepth = 0;
    if (loopDepth > 5) loopDepth = 5;
    return weights[loopDepth];
}