#ifndef RA_COALESCE_H
#define RA_COALESCE_H

#include "instr_selector.h"
#include "interference.h"

/**
 * @file ra_coalesce.h
 * @brief Biased coloring support (lightweight coalescing, no graph mutation).
 *
 * Unlike classic coalescing (which merges nodes in the interference graph
 * before coloring), this module only collects MOV-related vreg pairs.
 * ra_select_colors() (ra_color.c) uses these pairs as coloring *hints*:
 * when a vreg's move-partner already has a valid color, that color is
 * preferred, eliminating the redundant MOV without ever touching IGraph.
 *
 * Correctness: a hint is only followed if the partner's color already
 * belongs to the `available` mask computed by ra_select_colors (i.e. it
 * already satisfies interference edges, excl[], and crossesCall
 * constraints) — so no additional safety check is needed at hint time.
 */

/**
 * @brief One MOV-related pair of nodes considered for coalescing.
 *
 * Physical registers are represented in the same id space as
 * interference-graph nodes: node id = nextVreg + physReg.
 */
typedef struct {
    int u;   /**< First node id (vreg, always < nextVreg). */
    int v;   /**< Second node id: vreg id, or nextVreg+physReg for a physical register. */
} MovePair;

/**
 * @brief Growable list of MovePair entries, heap-allocated.
 */
typedef struct {
    MovePair *pairs; /**< Heap-allocated array of collected pairs. */
    int       count;  /**< Number of pairs currently stored. */
    int       cap;    /**< Allocated capacity of @c pairs. */
} MoveList;

/**
 * @brief Scan @p f for MOV instructions and collect non-interfering vreg pairs.
 *
 * Only MACH_MOV instructions between vreg<->vreg or vreg<->physical register
 * are considered. A pair is recorded only if the two nodes do NOT interfere
 * in @p g (checked via the interference bit matrix) — interfering nodes can
 * never receive the same color regardless of hinting.
 *
 * @param f        Machine function to scan.
 * @param g        Interference graph already built for @p f (used only for
 *                 the interference query; not modified).
 * @param nextVreg Number of virtual registers in @p f (id offset for physicals).
 * @return         Heap-allocated MoveList; caller must release with movelist_free().
 */
MoveList ra_collect_moves(const MachFunction *f, const IGraph *g, int nextVreg);

/**
 * @brief Free the backing array of @p ml and reset it to an empty state.
 *
 * @param ml MoveList to release (safe on an already-empty list).
 */
void movelist_free(MoveList *ml);

#endif /* RA_COALESCE_H */