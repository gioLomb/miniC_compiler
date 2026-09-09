#ifndef SR_H
#define SR_H


/**
 * @file sr.h
 * @brief Strength reduction loop optimization pass interface.
 */

#include "ir.h"

/** Maximum basic induction variables tracked per loop. */
#define MAX_IVARS   16

/** Maximum derived induction variables tracked per loop. */
#define MAX_DERIVED 64

/**
 * @brief Performs strength reduction on all natural loops in an IR function.
 *
 * Identifies basic and derived induction variables within each loop and rewrites
 * multiplications as incremental additions initialized in the loop pre-header.
 *
 * @param f Pointer to the IR function to optimize.
 * @return 1 if at least one transformation was applied, 0 otherwise.
 */
int sr_optimize(IRFunction *f,Arena *arena);

#endif /* SR_H */
