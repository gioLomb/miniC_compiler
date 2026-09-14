#ifndef REG_CLASS_H
#define REG_CLASS_H

/**
 * @file reg_class.h
 * @brief Register class abstraction: the regalloc pipeline (liveness,
 *        interference, coloring, spill) runs twice per function — once for
 *        integer vregs against GPRs, once for float vregs against XMM regs —
 *        without duplicating that logic.
 */

#include "instr_selector.h"

typedef enum { RC_INT = 0, RC_FLOAT = 1, RC_COUNT } RegClass;

/**
 * @brief Per-class constants needed by every generic regalloc stage.
 */
typedef struct {
    int allocatable;      /**< # of colorable physical regs in this class.   */
    int callerSavedCount; /**< # of caller-saved regs, counted from color 0. */
    int physBase;         /**< MachPhysReg value of local color 0.           */
} RegClassInfo;

static inline const RegClassInfo *reg_class_info(RegClass cls) {
    static const RegClassInfo INFO[RC_COUNT] = {
        [RC_INT]   = { PHYS_ALLOCATABLE, PHYS_CALLER_SAVED_COUNT, 0 },
        /* SysV: every XMM is caller-saved, so callerSavedCount == allocatable.
         * A float vreg live across a CALL finds available == 0 and is forced
         * to spill — reuses ig_apply_constraint_masks verbatim. */
        [RC_FLOAT] = { PHYS_XMM_COUNT, PHYS_XMM_COUNT, PHYS_XMM0 },
    };
    return &INFO[cls];
}

/** Map a local color (0..allocatable-1) to a MachPhysReg. */
static inline int reg_class_phys(RegClass cls, int localColor) {
    return reg_class_info(cls)->physBase + localColor;
}

#endif /* REG_CLASS_H */
