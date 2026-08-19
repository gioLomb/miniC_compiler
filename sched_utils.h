/**
 * @file sched_utils.h
 * @brief Utility predicates and operand extractors for the instruction scheduler.
 */

#ifndef SCHED_UTILS_H
#define SCHED_UTILS_H

#include "instr_selector.h"

/* =========================================================================
 * Latency table — Agner Fog Haswell/Broadwell estimates
 * ========================================================================= */

static inline int sched_latency_of(MachOp op) {
    switch (op) {
    case MACH_ADD: case MACH_SUB: case MACH_NEG: case MACH_NOT:
    case MACH_XOR:  return 1;
    case MACH_IMUL: return 3;
    case MACH_IDIV: return 20;
    case MACH_SAL:  return 1;
    case MACH_MOV: case MACH_MOVSX: return 1;
    case MACH_LOAD: case MACH_STORE:
    case MACH_PUSH: case MACH_POP:  return 4;
    case MACH_CMP: case MACH_TEST:  return 1;
    case MACH_SETE: case MACH_SETNE:
    case MACH_SETL: case MACH_SETLE:
    case MACH_SETG: case MACH_SETGE: return 1;
    case MACH_JMP:
    case MACH_JE:  case MACH_JNE:
    case MACH_JL:  case MACH_JLE:
    case MACH_JG:  case MACH_JGE:  return 1;
    case MACH_CALL: case MACH_RET: return 3;
    case MACH_CQO:  return 1;
    case MACH_LEA:  return 1;   /* leaq: address-generation unit, 1 cycle */
    case MACH_LABEL: case MACH_FUNC_BEGIN: case MACH_FUNC_END: return 0;
    default: return 1;
    }
}

/* =========================================================================
 * Instruction predicates
 * ========================================================================= */

static inline int sched_is_pinned(MachOp op) {
    switch (op) {
    case MACH_LABEL: case MACH_FUNC_BEGIN: case MACH_FUNC_END:
    case MACH_JMP:
    case MACH_JE:  case MACH_JNE:
    case MACH_JL:  case MACH_JLE:
    case MACH_JG:  case MACH_JGE:
    case MACH_RET: return 1;
    default:       return 0;
    }
}

static inline int sched_has_side_effect(MachOp op) {
    switch (op) {
    case MACH_STORE: case MACH_PUSH: case MACH_POP:
    case MACH_CALL:  case MACH_IDIV: case MACH_CQO: return 1;
    default:                                          return 0;
    }
}

static inline int sched_is_jcc(MachOp op) {
    switch (op) {
    case MACH_JE: case MACH_JNE:
    case MACH_JL: case MACH_JLE:
    case MACH_JG: case MACH_JGE: return 1;
    default:                       return 0;
    }
}

static inline int sched_is_cmp_or_test(MachOp op) {
    return op == MACH_CMP || op == MACH_TEST;
}

/* =========================================================================
 * Operand / def-use extractors
 * ========================================================================= */

static inline int sched_reg(const MachOperand *o, int nextVreg) {
    switch (o->kind) {
    case MO_VREG: return o->vregId;
    case MO_PHYS: return nextVreg + ((o->physReg == PHYS_AL) ? PHYS_RAX : o->physReg);
    case MO_MEM:  return (o->mem.baseVreg  >= 0) ? o->mem.baseVreg  : -1;
    default:      return -1;
    }
}

static inline int sched_reg_idx(const MachOperand *o) {
    return (o->kind == MO_MEM && o->mem.indexVreg >= 0) ? o->mem.indexVreg : -1;
}

static inline int sched_def(const MachInstr *in, int nextVreg) {
    switch (in->op) {
    case MACH_CMP:  case MACH_TEST:
    case MACH_JMP:
    case MACH_JE:   case MACH_JNE:
    case MACH_JL:   case MACH_JLE:
    case MACH_JG:   case MACH_JGE:
    case MACH_CALL: case MACH_RET:
    case MACH_PUSH: case MACH_STORE:
    case MACH_CQO:
    case MACH_LABEL: case MACH_FUNC_BEGIN: case MACH_FUNC_END:
        return -1;
    default:
        return sched_reg(&in->dst, nextVreg);
    }
}

static inline void sched_uses(const MachInstr *in, int nextVreg,
                               int out[], int *n) {
    *n = 0;
    int r;
#define SCHED_TRY(x) if ((r = (x)) >= 0) out[(*n)++] = r
    SCHED_TRY(sched_reg    (&in->src1, nextVreg));
    SCHED_TRY(sched_reg_idx(&in->src1));
    SCHED_TRY(sched_reg    (&in->src2, nextVreg));
    SCHED_TRY(sched_reg_idx(&in->src2));
    switch (in->op) {
    case MACH_STORE:
        SCHED_TRY(sched_reg    (&in->dst, nextVreg));
        SCHED_TRY(sched_reg_idx(&in->dst));
        break;
    case MACH_PUSH:
    case MACH_IDIV:
    case MACH_CQO:
        SCHED_TRY(sched_reg(&in->dst, nextVreg));
        break;
    default: break;
    }
#undef SCHED_TRY
}

#endif /* SCHED_UTILS_H */