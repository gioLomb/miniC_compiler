#ifndef RA_COLOR_H
#define RA_COLOR_H

/**
 * @file ra_color.h
 * @brief Chaitin-Briggs graph coloring: Simplify + Select phases.
 *
 * Both phases are parameterized by k (number of allocatable colors) and
 * callerSavedCount so the same code colors GPRs (k=14) and XMMs (k=8).
 */

#include "interference.h"
#include "bucket.h"
#include "ra_coalesce.h"

/**
 * @brief Simplify phase: iteratively remove low-degree nodes from @p g.
 *
 * @param g              Interference graph.
 * @param classVregCount Number of virtual registers in this class.
 * @param k              Number of allocatable physical colors.
 * @param outStack       Newly malloc'd removal-order array (caller frees).
 * @return               Number of entries in @p outStack.
 */
int ra_simplify(IGraph *g, int classVregCount, int k, int **outStack);

/**
 * @brief Select phase: assign colors in reverse removal order.
 *
 * @param g                Interference graph (colors written into g->color).
 * @param stack            Removal-order stack from ra_simplify().
 * @param stackLen         Length of @p stack.
 * @param k                Number of allocatable physical colors.
 * @param callerSavedCount Colors [0, callerSavedCount) are caller-saved.
 * @param spilled          Output array of uncolorable vreg ids.
 * @param pl               Partner list for biased coloring (may be NULL).
 * @return                 Number of spilled nodes.
 */
int ra_select_colors(IGraph *g, int *stack, int stackLen, int k, int callerSavedCount,
                     int *spilled, const PartnerList *pl);

#endif /* RA_COLOR_H */
