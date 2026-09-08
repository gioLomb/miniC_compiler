#ifndef REGALLOC_H
#define REGALLOC_H

#include "instr_selector.h"
#include "liveness.h"
#include "interference.h"
#include "regalloc_utils.h"

#define INITIAL_BLOCK_CAPACITY 8
#define STACK_ALIGNMENT_BYTES 16

/**
 * @file regalloc.h
 * @brief Register allocation via graph coloring (Chaitin-Briggs, EaC §13.4).
 *
 * @pre  @p mp is the result of is_isel_select() followed by sched_schedule():
 *       every value lives in a virtual register (MO_VREG) and frame size
 *       is not yet finalised.
 * @post Every MO_VREG has been replaced by MO_PHYS (physical register) or
 *       MO_STACK (spill slot); MachFunction.frameSize reflects the spill
 *       area; prologue/epilogue save and restore the callee-saved
 *       registers actually used.
 *
 * ### Techniques
 * - Interference graph: triangular bit matrix + adjacency lists.
 * - Simplify: Briggs-optimistic, bucket-by-degree removal, amortised O(1).
 * - Spill cost: weighted by loop nesting depth (10^loopDepth).
 * - Reload cache: reuses the same temporary across consecutive reads of the
 *   same spilled slot within a block (invalidated on label/jump/call).
 *
 * ### Known limitations
 * - No copy coalescing (EaC §13.4.3): a redundant reg->reg MOV may survive
 *   coloring. Correct, only suboptimal.
 */

/**
 * @brief Run register allocation on every function in @p mp.
 *
 * Allocates registers independently for each function via graph coloring,
 * inserting spill/reload code and callee-saved prologue/epilogue as needed.
 * Functions are modified in place.
 *
 * @param mp  Machine-level program to allocate registers for; must be the
 *            output of is_isel_select() + sched_schedule() (see @pre above).
 */
void regalloc(MachProgram *mp);

#endif /* REGALLOC_H */