#ifndef SR_H
#define SR_H

#include "ir.h"

/** Maximum basic induction variables tracked per loop. */
#define MAX_IVARS   16

/** Maximum derived induction variables tracked per loop. */
#define MAX_DERIVED 64

/**
 * @file sr.h
 * @brief Strength reduction loop optimization pass interface.
 *
 * Replaces expensive multiplication operations involving induction variables with
 * cheaper iterative additions inside natural loops.
 *
 * Overview
 * --------
 * For every loop containing a basic induction variable (e.g. `i = i + c`) and a
 * derived induction variable (e.g. `t = i * d`), this pass rewrites the IR as follows:
 *
 * @code
 *   [pre-header]          [pre-header]
 *                   ->      t_sr = i * d
 *
 *   [body]                [body]
 *     t = i * d     ->      t = t_sr        (candidate for Dead Code Elimination)
 *     i = i + c             i = i + c
 *                           t_sr = t_sr + (c * d)
 * @endcode
 *
 * Prerequisites
 * -------------
 * Requires `licm_optimize()` to have been executed on the target function beforehand
 * so that loop pre-headers exist and invariant statements have been hoisted.
 */

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
