/**
 * @file regalloc_utils.h
 * @brief Instruction-analysis utilities shared across the register allocator.
 *
 * Provides three groups of functions used by regalloc.c, interference.c,
 * ra_spill.c, and sched_utils.h:
 *
 *  1. Spill-cost weighting
 *     regalloc_spill_weight() maps a static loop-nesting depth to an integer
 *     weight (10^depth), so that variables live inside hot loops are preferred
 *     for register assignment over variables that live only in cold code.
 *
 *  2. Operand extraction — explicit and implicit
 *     x86-64 instructions have operands beyond what is visible in the
 *     MachOperand fields: IDIV implicitly reads and writes RAX/RDX; CALL
 *     clobbers all caller-saved registers.  The extraction functions make
 *     these implicit data-flow edges explicit so that liveness analysis and
 *     the interference graph see the complete use-def information.
 *
 *     instr_uses()          — explicit source operands (registers read)
 *     instr_implicit_uses() — implicit reads (e.g. RAX/RDX before IDIV)
 *     instr_defs()          — explicit destination operand (register written)
 *     instr_implicit_defs() — implicit writes (e.g. caller-saved after CALL)
 *
 *  3. Instruction predicates
 *     regalloc_is_rmw()          — read-modify-write: destination is also a source
 *     regalloc_is_setcc()        — SETcc family: result goes into %al
 *     regalloc_is_ctrl_transfer() — control-flow: cannot be moved or deleted
 *
 * Physical-register indexing convention
 * --------------------------------------
 * Physical registers are represented as integers in the range
 * [nextVreg, nextVreg + PHYS_ALLOCATABLE).  The offset @c nextVreg is passed
 * explicitly so that the functions are independent of the virtual-register
 * count of any particular function.
 */

#ifndef REGALLOC_UTILS_H
#define REGALLOC_UTILS_H

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