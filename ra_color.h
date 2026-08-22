#ifndef RA_COLOR_H
#define RA_COLOR_H

#include "interference.h"
#include "bucket.h"
#include "ra_coalesce.h"

/**
 * @file ra_color.h
 * @brief Chaitin-Briggs graph coloring: Simplify + Select phases.
 *
 * Two-stage register allocation over an already-built IGraph:
 *
 *  1. ra_simplify() — repeatedly removes nodes from the graph in
 *     bucket-by-degree order (O(1) amortised via bucket.h), pushing each
 *     removed node onto a stack. Uses Briggs-optimistic heuristic for
 *     potential spill candidates: a node with degree >= k is not
 *     immediately marked as spill, it is only assumed *possibly*
 *     colorable and picked by lowest spill-cost/degree ratio when no
 *     node with degree < k remains.
 *
 *  2. ra_select_colors() — pops the stack in reverse order and assigns a
 *     physical-register color to each node. Applies biased coloring: if
 *     a move-related partner (from PartnerList) already has an available
 *     color, that color is preferred, eliminating the redundant MOV.
 *     Nodes with no available color are recorded in @c spilled[].
 *     Vregs live across a CALL prefer callee-saved registers to reduce
 *     push/pop overhead in the prologue/epilogue.
 *
 * The caller (regalloc.c) owns allocation/deallocation of stack/spilled.
 */

/**
 * @brief Simplify phase: iteratively remove low-degree nodes from @p g.
 *
 * Nodes with degree < k = PHYS_ALLOCATABLE are always safe to remove
 * (they are guaranteed colorable once their neighbours are colored).
 * When no such node exists, an optimistic spill candidate is chosen by
 * minimising spillCost/degree among remaining active nodes not already
 * bucketed — this node may still turn out colorable in ra_select_colors().
 *
 * @param g        Interference graph (nodes are progressively deactivated).
 * @param nextVreg Number of virtual registers (graph node range [0, nextVreg)).
 * @param outStack Set to a newly malloc'd array (caller must free) holding
 *                 the removal order; used by ra_select_colors() in reverse.
 * @return         Number of entries written into @p outStack.
 */
int ra_simplify(IGraph *g, int nextVreg, int **outStack);

/**
 * @brief Select phase: assign physical-register colors in reverse removal order.
 *
 * For each node popped from @p stack (LIFO reinsertion), computes the set of
 * colors still available given already-colored neighbours and the node's
 * exclusion mask, then applies the following priority:
 *
 *  1. Biased hint from a move-related partner in @p pl (if available and
 *     still valid within the @c available mask).
 *  2. For vregs crossing a CALL, prefer a callee-saved color to minimise
 *     push/pop overhead in the function prologue/epilogue.
 *  3. Lowest available color (no special preference).
 *
 * If no color is available the node is marked as spilled (@c color = -2)
 * and appended to @p spilled.
 *
 * @param g        Interference graph (colors written into g->color).
 * @param nextVreg Number of virtual registers.
 * @param stack    Removal-order stack produced by ra_simplify().
 * @param stackLen Number of entries in @p stack.
 * @param spilled  Output array (caller-allocated, capacity >= nextVreg)
 *                 receiving the ids of nodes that could not be colored.
 * @param pl       Partner list for biased coloring hints produced by
 *                 ra_collect_partners(); may be NULL, in which case
 *                 coloring proceeds without any hints.
 * @return         Number of entries written into @p spilled.
 */
int ra_select_colors(IGraph *g, int nextVreg, int *stack, int stackLen,
                     int *spilled, const PartnerList *pl);

#endif /* RA_COLOR_H */