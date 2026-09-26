#ifndef SR_H
#define SR_H


/**
 * @file sr.h
 * @brief Strength reduction loop optimization pass interface.
 *
 * Identifies induction variables and expensive operations (multiplications,
 * array-index calculations) inside loops and replaces them by cheaper
 * equivalent sequences of additions, thereby reducing the cost of each
 * iteration.
 */

#include "ir.h"
#include "arena.h"
#include "varmap.h"

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
 * @param arena Scratch arena for dominators / loop descriptors.
 * @param sharedVm Optional shared VarMap (NULL → private, destroyed on return).
 * @return 1 if at least one transformation was applied, 0 otherwise.
 */
int sr_optimize(IRFunction *f, Arena *arena, VarMap *sharedVm);

#endif /* SR_H */
