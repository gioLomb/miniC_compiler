#include <stdlib.h>
#include <stdint.h>
#include "ra_coalesce.h"

static inline int ra_resolve_operand_id(const MachOperand *op, RegClass cls, int classVregCount) {
    if (cls == RC_INT) {
        if (op->kind == MO_VREG) return op->vregId;
        if (op->kind == MO_PHYS && op->physReg < PHYS_XMM0)
            return classVregCount + ((op->physReg == PHYS_AL) ? PHYS_RAX : op->physReg);
    } else {
        if (op->kind == MO_VREG_F) return op->vregId;
        if (op->kind == MO_PHYS && op->physReg >= PHYS_XMM0 &&
            op->physReg < PHYS_XMM0 + PHYS_XMM_COUNT)
            return classVregCount + (op->physReg - PHYS_XMM0);
    }
    return -1;
}

static inline int ra_is_valid_coalesce_candidate(const IGraph *g, int aVregId, int aPartnerId,
                                                 int max_node_id, int classVregCount) {
    if (aVregId < 0 || aPartnerId < 0) return 0;
    if (aVregId >= max_node_id || aPartnerId >= max_node_id) return 0;
    if (aVregId >= classVregCount && aPartnerId >= classVregCount) return 0; /* phys↔phys */
    if (ig_has_edge(g, aVregId, aPartnerId)) return 0;
    return 1;
}

PartnerList ra_collect_partners(const MachFunction *f, const IGraph *g,
                                RegClass cls, int classVregCount) {
    PartnerList pl = { NULL, 0, 0 };
    const RegClassInfo *ci = reg_class_info(cls);
    const int max_node_id = classVregCount + ci->allocatable;
    const MachOpCode wantOp = (cls == RC_FLOAT) ? MACH_MOVSS : MACH_MOV;

    for (int i = 0; i < f->count; i++) {
        const MachInstr *in = &f->instrs[i];
        if (in->op != wantOp) continue;  // only move instructions are coalesce candidates

        int aVregId = ra_resolve_operand_id(&in->dst,  cls, classVregCount);
        int aPartnerId = ra_resolve_operand_id(&in->src1, cls, classVregCount);

        // vreg side always becomes .vregId; dst is the vreg unless it's a
        // physical register and src1 is the actual vreg operand
        // int dstIsVreg   = dstNode >= 0 && dstNode < classVregCount;
        // int aVregId     = dstIsVreg ? dstNode : srcNode;
        // int aPartnerId  = dstIsVreg ? srcNode : dstNode;
    
         /* Prefer recording (vreg, partner) with vreg as u. */
       if (aVregId >= classVregCount && aPartnerId < classVregCount) {
           int t = aVregId; aVregId = aPartnerId;aPartnerId = t;
        }
        if (ra_is_valid_coalesce_candidate(g, aVregId, aPartnerId, max_node_id, classVregCount)) {
            if (pl.count == pl.cap) {
                pl.cap = pl.cap ? pl.cap * 2 : 8;
                pl.pairs = realloc(pl.pairs, (size_t)pl.cap * sizeof(PartnerPair));
            }
            pl.pairs[pl.count++] = (PartnerPair){ .vregId = aVregId, .partnerId = aPartnerId };
        }
    }
    return pl;
}

void partnerlist_free(PartnerList *pl) {
    if (!pl) return;
    free(pl->pairs);
    pl->pairs = NULL;
    pl->count = 0;
    pl->cap   = 0;
}