#include "regalloc_utils.h"

static inline int regalloc_normalize_phys(int p) {
    return (p == PHYS_AL) ? PHYS_RAX : p;
}

int regalloc_spill_weight(int loopDepth) {
    static const int weights[] = {1, 10, 100, 1000, 10000, 100000};
    if (loopDepth < 0) loopDepth = 0;
    if (loopDepth > 5) loopDepth = 5;
    return weights[loopDepth];
}

static inline int regalloc_operand_reg(const MachOperand *o) {
    switch (o->kind) {
    case MO_VREG: return o->vregId;
    case MO_PHYS: return regalloc_normalize_phys(o->physReg);
    case MO_MEM:  return (o->mem.baseVreg >= 0) ? o->mem.baseVreg : -1;
    default:      return -1;
    }
}

static inline int regalloc_operand_reg2(const MachOperand *o) {
    if (o->kind == MO_MEM && o->mem.indexVreg >= 0)
        return o->mem.indexVreg;
    return -1;
}

int instr_def(const MachInstr *in, int nextVreg) {
    (void)nextVreg;
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
        return regalloc_operand_reg(&in->dst);
    }
}

void instr_uses(const MachInstr *in, int nextVreg, int out[], int *n) {
    (void)nextVreg;
    *n = 0;
    int r;
    r = regalloc_operand_reg(&in->src1); if (r >= 0) out[(*n)++] = r;
    r = regalloc_operand_reg2(&in->src1); if (r >= 0) out[(*n)++] = r;
    r = regalloc_operand_reg(&in->src2); if (r >= 0) out[(*n)++] = r;
    r = regalloc_operand_reg2(&in->src2); if (r >= 0) out[(*n)++] = r;
    switch (in->op) {
    case MACH_STORE: case MACH_PUSH: case MACH_IDIV: case MACH_CQO:
        r = regalloc_operand_reg(&in->dst); if (r >= 0) out[(*n)++] = r;
        break;
    default: break;
    }
}

void instr_implicit_uses(const MachInstr *in, int nextVreg, int out[], int *n) {
    *n = 0;
    switch (in->op) {
    case MACH_IDIV: out[(*n)++] = nextVreg + PHYS_RAX; out[(*n)++] = nextVreg + PHYS_RDX; break;
    case MACH_CQO:  out[(*n)++] = nextVreg + PHYS_RAX; break;
    case MACH_CALL:
        for (int p = 0; p < PHYS_CALLER_SAVED_COUNT; p++) out[(*n)++] = nextVreg + p;
        break;
    case MACH_RET:  out[(*n)++] = nextVreg + PHYS_RAX; break;
    default: break;
    }
}

void instr_implicit_defs(const MachInstr *in, int nextVreg, int out[], int *n) {
    *n = 0;
    switch (in->op) {
    case MACH_IDIV: out[(*n)++] = nextVreg + PHYS_RAX; out[(*n)++] = nextVreg + PHYS_RDX; break;
    case MACH_CQO:  out[(*n)++] = nextVreg + PHYS_RDX; break;
    case MACH_CALL:
        for (int p = 0; p < PHYS_CALLER_SAVED_COUNT; p++) out[(*n)++] = nextVreg + p;
        break;
    default: break;
    }
}

void instr_defs(const MachInstr *in, int nextVreg, int out[], int *n) {
    *n = 0;
    int id = instr_def(in, nextVreg);
    if (id >= 0) out[(*n)++] = id;
}

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
    case MACH_SETE: case MACH_SETNE: case MACH_SETL:
    case MACH_SETLE: case MACH_SETG: case MACH_SETGE:
        return 1;
    default: return 0;
    }
}

int regalloc_is_ctrl_transfer(MachOp op) {
    switch (op) {
    case MACH_JMP: case MACH_JE: case MACH_JNE: case MACH_JL:
    case MACH_JLE: case MACH_JG: case MACH_JGE:
    case MACH_CALL: case MACH_RET:
        return 1;
    default: return 0;
    }
}
