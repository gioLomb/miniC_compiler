/**
 * @file regalloc_utils.c
 * @brief Instruction-analysis utilities — implementation.
 *
 * See regalloc_utils.h for the module overview and full API documentation.
 */

#include "regalloc_utils.h"

/* =========================================================================
 * Spill-cost weighting
 * ========================================================================= */

int regalloc_spill_weight(int loopDepth) {
    // precomputed powers of 10 indexed by depth; depth is clamped to [0, 5]
    static const int weights[] = { 1, 10, 100, 1000, 10000, 100000 };
    if (loopDepth < 0) loopDepth = 0;
    if (loopDepth > 5) loopDepth = 5;
    return weights[loopDepth];
}

/* =========================================================================
 * Internal operand-to-register helpers
 * ========================================================================= */

/**
 * @brief Extract the primary register id from a MachOperand, or -1 if none.
 *
 * For MO_VREG returns the virtual register id directly.
 * For MO_PHYS normalises %al to %rax (they share the architectural register).
 * For MO_MEM returns the base register id (index is extracted separately by
 * regalloc_operand_reg2).
 * All other kinds (immediate, label, func, stack, none) return -1.
 */
static inline int regalloc_operand_reg(const MachOperand *o) {
    switch (o->kind) {
    case MO_VREG: return o->vregId;
    case MO_PHYS: return (o->physReg == PHYS_AL) ? PHYS_RAX : o->physReg; // %al aliases %rax
    case MO_MEM:  return (o->mem.baseVreg >= 0) ? o->mem.baseVreg : -1;
    default:      return -1;
    }
}

/**
 * @brief Extract the index register of a MO_MEM operand, or -1 if none.
 *
 * SIB addressing can use a separate index register that is also read by the
 * instruction and must appear in the use set for correct liveness.
 */
static inline int regalloc_operand_reg2(const MachOperand *o) {
    if (o->kind == MO_MEM && o->mem.indexVreg >= 0)
        return o->mem.indexVreg;
    return -1;
}

/* =========================================================================
 * Operand extraction — explicit
 * ========================================================================= */

int instr_def(const MachInstr *in, int nextVreg) {
    (void)nextVreg; // physical registers are handled by regalloc_operand_reg directly
    switch (in->op) {
    // instructions that do not write a destination register
    case MACH_CMP:  case MACH_TEST:
    case MACH_JMP:  case MACH_JE:   case MACH_JNE:
    case MACH_JL:   case MACH_JLE:  case MACH_JG:  case MACH_JGE:
    case MACH_CALL: case MACH_RET:
    case MACH_PUSH: case MACH_STORE:
    case MACH_CQO:
    case MACH_LABEL: case MACH_FUNC_BEGIN: case MACH_FUNC_END:
        return -1;
    default:
        return regalloc_operand_reg(&in->dst);
    }
}

void instr_uses(const MachInstr *in, int nextVreg, int out[], int *n) {
    (void)nextVreg;
    *n = 0;
    int r;
    // src1: primary register and index register (for SIB addressing)
    r = regalloc_operand_reg(&in->src1);  if (r >= 0) out[(*n)++] = r;
    r = regalloc_operand_reg2(&in->src1); if (r >= 0) out[(*n)++] = r;
    // src2: same
    r = regalloc_operand_reg(&in->src2);  if (r >= 0) out[(*n)++] = r;
    r = regalloc_operand_reg2(&in->src2); if (r >= 0) out[(*n)++] = r;
    // for RMW-like ops the dst field is also read before being written
    switch (in->op) {
    case MACH_STORE: // base address in dst is read
    case MACH_PUSH:  // value in dst is read
    case MACH_IDIV:  // divisor in dst is read
    case MACH_CQO:   // not really dst but treated uniformly
        r = regalloc_operand_reg(&in->dst); if (r >= 0) out[(*n)++] = r;
        break;
    default: break;
    }
}

/* =========================================================================
 * Operand extraction — implicit (architectural side effects)
 * ========================================================================= */

void instr_implicit_uses(const MachInstr *in, int nextVreg, int out[], int *n) {
    *n = 0;
    switch (in->op) {
    case MACH_IDIV:
        // IDIV reads RDX:RAX as the 128-bit dividend
        out[(*n)++] = nextVreg + PHYS_RAX;
        out[(*n)++] = nextVreg + PHYS_RDX;
        break;
    case MACH_CQO:
        // CQO sign-extends RAX into RDX; RAX is the input
        out[(*n)++] = nextVreg + PHYS_RAX;
        break;
    case MACH_CALL:
        // conservatively model all caller-saved registers as used by the call
        // (the callee may read any of them as arguments)
        for (int p = 0; p < PHYS_CALLER_SAVED_COUNT; p++)
            out[(*n)++] = nextVreg + p;
        break;
    case MACH_RET:
        // return value is in RAX; model it as used so the allocator keeps it live
        out[(*n)++] = nextVreg + PHYS_RAX;
        break;
    default: break;
    }
}

void instr_implicit_defs(const MachInstr *in, int nextVreg, int out[], int *n) {
    *n = 0;
    switch (in->op) {
    case MACH_IDIV:
        // IDIV writes quotient to RAX and remainder to RDX
        out[(*n)++] = nextVreg + PHYS_RAX;
        out[(*n)++] = nextVreg + PHYS_RDX;
        break;
    case MACH_CQO:
        // CQO writes the sign extension into RDX
        out[(*n)++] = nextVreg + PHYS_RDX;
        break;
    case MACH_CALL:
        // all caller-saved registers are clobbered by the call
        for (int p = 0; p < PHYS_CALLER_SAVED_COUNT; p++)
            out[(*n)++] = nextVreg + p;
        break;
    default: break;
    }
}

void instr_defs(const MachInstr *in, int nextVreg, int out[], int *n) {
    *n = 0;
    int id = instr_def(in, nextVreg);
    if (id >= 0) out[(*n)++] = id;
}

/* =========================================================================
 * Instruction predicates
 * ========================================================================= */

int regalloc_is_rmw(MachOp op) {
    switch (op) {
    case MACH_ADD: case MACH_SUB: case MACH_IMUL:
    case MACH_SAL: case MACH_NEG: case MACH_NOT: case MACH_XOR:
        return 1;
    default: return 0;
    }
}

int regalloc_is_setcc(MachOp op) {
    switch (op) {
    case MACH_SETE:  case MACH_SETNE:
    case MACH_SETL:  case MACH_SETLE:
    case MACH_SETG:  case MACH_SETGE:
        return 1;
    default: return 0;
    }
}

int regalloc_is_ctrl_transfer(MachOp op) {
    switch (op) {
    case MACH_JMP: case MACH_JE:  case MACH_JNE:
    case MACH_JL:  case MACH_JLE: case MACH_JG: case MACH_JGE:
    case MACH_CALL: case MACH_RET:
        return 1;
    default: return 0;
    }
}