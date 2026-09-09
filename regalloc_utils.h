

#ifndef REGALLOC_UTILS_H
#define REGALLOC_UTILS_H


/**
 * @file regalloc_utils.h
 * @brief Instruction-analysis utilities shared across the register allocator.
 */

#include "block.h"
#include "instr_selector.h"

/* =========================================================================
 * Spill-cost weighting
 * ========================================================================= */

/**
 * @brief Return the spill cost weight for a given static loop-nesting depth.
 *
 * Uses the classic 10^depth heuristic: a variable live inside one loop is
 * 10× more expensive to spill than one outside; inside two nested loops, 100×;
 * and so on.  The depth is clamped to [0, 5] to avoid overflow.
 *
 * @param loopDepth  Static nesting depth of the instruction (from IRInstr.loopDepth).
 * @return           Spill weight (1, 10, 100, 1 000, 10 000, or 100 000).
 */
int regalloc_spill_weight(int loopDepth); //TODO: INTERFERENCE

#endif /* REGALLOC_UTILS_H */