#ifndef RA_COALESCE_H
#define RA_COALESCE_H


/**
 * @file ra_coalesce.h
 * @brief Biased-coloring support: partner list collection (lightweight coalescing).
 */

#include "instr_selector.h"
#include "interference.h"

/**
 * @brief An ordered pair of nodes considered for biased coloring.
 *
 * Physical registers share the same id space as interference-graph nodes:
 * physical register @c p has node id @c nextVreg + p.
 * @c u is always a virtual register (@c u < nextVreg); @c v may be either a
 * virtual register or a pre-coloured physical-register node.
 */
typedef struct {
    int u;  /**< Source node id — always a virtual register (u < nextVreg). */
    int v;  /**< Partner node id — vreg id or nextVreg+physReg for a physical reg. */
} PartnerPair;

/**
 * @brief Growable list of PartnerPair entries, heap-allocated.
 *
 * Managed with a standard doubling-growth strategy.  All memory is released
 * by partnerlist_free(); the struct itself is stack-allocated by the caller.
 */
typedef struct {
    PartnerPair *pairs;  /**< Heap-allocated array of partner pairs.      */
    int          count;  /**< Number of valid entries currently stored.   */
    int          cap;    /**< Allocated capacity of @c pairs (in entries). */
} PartnerList;

/**
 * @brief Scan @p f for MOV instructions and collect non-interfering vreg pairs.
 *
 * Considers MACH_MOV instructions between vreg↔vreg or vreg↔physical-register
 * operands.  A pair is recorded only when the two nodes do NOT already interfere
 * in @p g — interfering nodes can never share a color regardless of any hint,
 * so recording them would waste memory and hint-lookup time.
 *
 * @param f        Machine function to scan.
 * @param g        Interference graph already built for @p f (read-only; used
 *                 only for edge-existence queries via the bit matrix).
 * @param nextVreg Number of virtual registers in @p f (id offset for physicals).
 * @return         Heap-allocated PartnerList; caller must release with
 *                 partnerlist_free().
 */
PartnerList ra_collect_partners(const MachFunction *f, const IGraph *g, int nextVreg);

/**
 * @brief Release the backing array of @p pl and reset it to an empty state.
 *
 * Safe to call on a PartnerList that was never populated (no-op on empty list).
 *
 * @param pl  PartnerList to release.
 */
void partnerlist_free(PartnerList *pl);

#endif /* RA_COALESCE_H */