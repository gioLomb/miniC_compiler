#include "regalloc_utils.h"

/* =========================================================================
 * Spill cost weighting
 * =========================================================================
 * Weight is 10^loopDepth (capped at depth 5 = 100 000) so that variables
 * live inside hot loops are strongly preferred for register allocation over
 * variables that are rarely executed. The register allocator sums these
 * weights across all def/use sites to build a per-vreg spill cost.
 * ========================================================================= */

int regalloc_spill_weight(int loopDepth) {
    static const int weights[] = {1, 10, 100, 1000, 10000, 100000};
    // clamp to the precomputed table range to avoid out-of-bounds access
    // and overflow for pathologically deep nesting
    if (loopDepth < 0) loopDepth = 0;
    if (loopDepth > 5) loopDepth = 5;
    return weights[loopDepth];
}

static inline int regalloc_operand_reg(const MachOperand *o, int nextVreg) {
    switch (o->kind) {
    case MO_VREG: return o->vregId;
    // same id scheme as sched_reg() (sched_utils.h): physical reg P -> id
    // (nextVreg + P), so physical and virtual ids never collide in the bitset
    case MO_PHYS: return nextVreg + ((o->physReg == PHYS_AL) ? PHYS_RAX : o->physReg);
    case MO_MEM:  return (o->mem.baseVreg >= 0) ? o->mem.baseVreg : -1;
    default:      return -1;
    }
}

// baseVreg/indexVreg in MO_MEM are always plain vreg ids, never physical
static inline int regalloc_operand_reg2(const MachOperand *o) {
    if (o->kind == MO_MEM && o->mem.indexVreg >= 0)
        return o->mem.indexVreg;
    return -1;
}

static inline void push_reg_id(int out[], int *n, int id) {
    if (id >= 0) out[(*n)++] = id;
}

// second id only present for MO_MEM (index register)
static inline void push_operand_regs(int out[], int *n, const MachOperand *o, int nextVreg) {
    push_reg_id(out, n, regalloc_operand_reg(o, nextVreg));
    push_reg_id(out, n, regalloc_operand_reg2(o));
}

int instr_def(const MachInstr *in, int nextVreg) {
    switch (in->op) {
    case MACH_CMP: case MACH_TEST:
    case MACH_JMP: case MACH_JE: case MACH_JNE:
    case MACH_JL:  case MACH_JLE: case MACH_JG: case MACH_JGE:
    case MACH_CALL: case MACH_RET:
    case MACH_PUSH: case MACH_STORE:
    case MACH_CQO:
    case MACH_LABEL: case MACH_FUNC_BEGIN: case MACH_FUNC_END:
        return -1;
    default:
        return regalloc_operand_reg(&in->dst, nextVreg);
    }
}

void instr_uses(const MachInstr *in, int nextVreg, int out[], int *n) {
    *n = 0;
    push_operand_regs(out, n, &in->src1, nextVreg);
    push_operand_regs(out, n, &in->src2, nextVreg);
    switch (in->op) {
    case MACH_STORE:
        push_operand_regs(out, n, &in->dst, nextVreg);
        break;
    case MACH_PUSH:
    case MACH_IDIV:
    case MACH_CQO:
        push_reg_id(out, n, regalloc_operand_reg(&in->dst, nextVreg));
        break;
    default:
        if (regalloc_is_rmw(in->op)) {
            push_reg_id(out, n, regalloc_operand_reg(&in->dst, nextVreg));
        }
        break;
    }
}


static inline void append_caller_saved(int out[], int *n, int nextVreg) {
    for (int p = 0; p < PHYS_CALLER_SAVED_COUNT; p++)
        out[(*n)++] = nextVreg + p;
}

static inline void append_rax_rdx(int out[], int *n, int nextVreg) {
    out[(*n)++] = nextVreg + PHYS_RAX;
    out[(*n)++] = nextVreg + PHYS_RDX;
}

void instr_implicit_uses(const MachInstr *in, int nextVreg, int out[], int *n) {
    *n = 0;
    switch (in->op) {
    case MACH_IDIV:
        /* IDIV reads RDX:RAX as the dividend */
        append_rax_rdx(out, n, nextVreg);
        break;
    case MACH_CQO:
        /* CQO sign-extends RAX into RDX:RAX; reads RAX */
        out[(*n)++] = nextVreg + PHYS_RAX;
        break;
    case MACH_CALL:
        /* All caller-saved registers are potentially clobbered */
        // modelled conservatively as reads too: the callee may read them
        // as incoming arguments, so they must be excluded from reuse
        // across the call regardless of direction
        append_caller_saved(out, n, nextVreg);
        break;
    case MACH_RET:
        /* RET reads the return value from RAX */
        out[(*n)++] = nextVreg + PHYS_RAX;
        break;
    default: break;
    }
}

void instr_implicit_defs(const MachInstr *in, int nextVreg, int out[], int *n) {
    *n = 0;
    switch (in->op) {
    case MACH_IDIV:
        /* IDIV writes quotient -> RAX, remainder -> RDX */
        append_rax_rdx(out, n, nextVreg);
        break;
    case MACH_CQO:
        /* CQO writes the sign-extension into RDX */
        out[(*n)++] = nextVreg + PHYS_RDX;
        break;
    case MACH_CALL:
        /* CALL clobbers all caller-saved registers */
        append_caller_saved(out, n, nextVreg);
        break;
    default: break;
    }
}


void instr_defs(const MachInstr *in, int nextVreg, int out[], int *n) {
    *n = 0;
    // thin wrapper: instr_def already returns -1 or exactly one id
    int id = instr_def(in, nextVreg);
    if (id >= 0) out[(*n)++] = id;
}


int regalloc_is_rmw(MachOpCode op) {
    switch (op) {
    // two-operand ALU ops where dst is both an input and the output
    case MACH_ADD: case MACH_SUB: case MACH_IMUL:
    case MACH_SAL: case MACH_NEG: case MACH_NOT: case MACH_XOR:
        return 1;
    default: return 0;
    }
}

int regalloc_is_setcc(MachOpCode op) {
    switch (op) {
    case MACH_SETE: case MACH_SETNE: case MACH_SETL:
    case MACH_SETLE: case MACH_SETG: case MACH_SETGE:
        return 1;
    default: return 0;
    }
}

int regalloc_is_ctrl_transfer(MachOpCode op) {
    switch (op) {
    // any instruction that can transfer control away from the next
    // sequential instruction invalidates same-block assumptions
    case MACH_JMP: case MACH_JE: case MACH_JNE: case MACH_JL:
    case MACH_JLE: case MACH_JG: case MACH_JGE:
    case MACH_CALL: case MACH_RET:
        return 1;
    default: return 0;
    }
}