#ifndef REGALLOC_H
#define REGALLOC_H


/**
 * @file regalloc.h
 * @brief Register allocation via graph coloring (Chaitin-Briggs, EaC §13.4).
 */

#include "instr_selector.h"
#include "liveness.h"
#include "interference.h"
#include "regalloc_utils.h"

#define INITIAL_BLOCK_CAPACITY 8
#define STACK_ALIGNMENT_BYTES 16

/**
 * @brief Run register allocation on every function in @p mp.
 *
 * Allocates registers independently for each function via graph coloring,
 * inserting spill/reload code and callee-saved prologue/epilogue as needed.
 * Functions are modified in place.
 *
 * @param mp  Machine-level program to allocate registers for; must be the
 *            output of isel_select() + sched_schedule() (see @pre above).
 */
void regalloc(MachProgram *mp);

#endif /* REGALLOC_H */