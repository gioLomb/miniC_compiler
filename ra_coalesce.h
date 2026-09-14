#ifndef RA_COALESCE_H
#define RA_COALESCE_H

/**
 * @file ra_coalesce.h
 * @brief Biased-coloring partner list (lightweight coalescing hints).
 */

#include "instr_selector.h"
#include "interference.h"
#include "reg_class.h"

typedef struct {
    int u;  /**< Virtual register id (u < classVregCount). */
    int v;  /**< Partner: vreg id or classVregCount+localColor for a phys. */
} PartnerPair;

typedef struct {
    PartnerPair *pairs;
    int          count;
    int          cap;
} PartnerList;

/**
 * @brief Collect non-interfering move pairs for class @p cls.
 *
 * RC_INT considers MACH_MOV with MO_VREG; RC_FLOAT considers MACH_MOVSS
 * with MO_VREG_F.  Physical partners use class-local color ids.
 */
PartnerList ra_collect_partners(const MachFunction *f, const IGraph *g,
                                RegClass cls, int classVregCount);

void partnerlist_free(PartnerList *pl);

#endif /* RA_COALESCE_H */
