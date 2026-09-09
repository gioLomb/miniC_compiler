#ifndef RA_COLOR_H
#define RA_COLOR_H


/**
 * @file ra_color.h
 * @brief Chaitin-Briggs graph coloring: Simplify + Select phases.
 */

#include "interference.h"
#include "bucket.h"
#include "ra_coalesce.h"

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
 * ...
 * @param g        Interference graph (colors written into g->color).
 * @param stack    Removal-order stack produced by ra_simplify().
 * @param stackLen Number of entries in @p stack.
 * @param spilled  Output array (caller-allocated, capacity >= nextVreg)
 *                 receiving the ids of nodes that could not be colored.
 * @param pl       Partner list for biased coloring hints produced by
 *                 ra_collect_partners(); may be NULL, in which case
 *                 coloring proceeds without any hints.
 * @return         Number of entries written into @p spilled.
 */
int ra_select_colors(IGraph *g, int *stack, int stackLen,
                     int *spilled, const PartnerList *pl);

#endif /* RA_COLOR_H */