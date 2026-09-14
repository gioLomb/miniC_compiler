/**
 * @file instr_query.c
 * @brief Class-aware MachInstr operand extraction and opcode predicates.
 */

#include "instr_query.h"

static inline int mach_operand_reg_c(const MachOperand *o, int classVregCount, RegClass cls) {
    switch (o->kind) {
    case MO_VREG:
        return (cls == RC_INT) ? o->vregId : -1;
    case MO_VREG_F:
        return (cls == RC_FLOAT) ? o->vregId : -1;
    case MO_PHYS:
        if (cls == RC_INT) {
            if (o->physReg >= PHYS_XMM0) return -1;
            // %al and %rax share one architectural register: normalise
            return classVregCount + ((o->physReg == PHYS_AL) ? PHYS_RAX : o->physReg);
        } else {
            if (o->physReg < PHYS_XMM0 || o->physReg >= PHYS_XMM0 + PHYS_XMM_COUNT)
                return -1;
            // re-base into the float class' own 0-based id space
            return classVregCount + (o->physReg - PHYS_XMM0);
        }
    case MO_MEM:
        return (cls == RC_INT && o->mem.baseVreg >= 0) ? o->mem.baseVreg : -1;
    default:
        return -1;
    }
}

static inline int mach_operand_reg2_c(const MachOperand *o, RegClass cls) {
    return (cls == RC_INT && o->kind == MO_MEM && o->mem.indexVreg >= 0)
               ? o->mem.indexVreg : -1;
}

static inline void push_operand_regs_c(int out[], int *n, const MachOperand *o,
                                       int classVregCount, RegClass cls) {
    int a = mach_operand_reg_c(o, classVregCount, cls);
    if (a >= 0) out[(*n)++] = a;
    int b = mach_operand_reg2_c(o, cls);
    if (b >= 0) out[(*n)++] = b;
}

int instr_def(const MachInstr *in, int classVregCount, RegClass cls) {
    switch (in->op) {
    case MACH_CMP: case MACH_TEST: case MACH_UCOMISS:
    case MACH_JMP: case MACH_JE: case MACH_JNE:
    case MACH_JL:  case MACH_JLE: case MACH_JG: case MACH_JGE:
    case MACH_JB:  case MACH_JBE: case MACH_JA: case MACH_JAE:
    case MACH_CALL: case MACH_RET:
    case MACH_PUSH: case MACH_STORE:
    case MACH_CQO:
    case MACH_LABEL: case MACH_FUNC_BEGIN: case MACH_FUNC_END:
        return -1;
    default:
        return mach_operand_reg_c(&in->dst, classVregCount, cls);
    }
}

void instr_uses(const MachInstr *in, int classVregCount, RegClass cls, int out[], int *n) {
    *n = 0;
    push_operand_regs_c(out, n, &in->src1, classVregCount, cls);
    push_operand_regs_c(out, n, &in->src2, classVregCount, cls);

    switch (in->op) {
    case MACH_STORE:
    case MACH_CMP:
    case MACH_TEST:
    case MACH_UCOMISS:
        // dst read-only here: STORE address, or CMP/TEST/UCOMISS lhs operand
        push_operand_regs_c(out, n, &in->dst, classVregCount, cls);
        break;
    case MACH_PUSH:
    case MACH_IDIV:
    case MACH_CQO: {
        int r = mach_operand_reg_c(&in->dst, classVregCount, cls);
        if (r >= 0) out[(*n)++] = r;
        break;
    }
    default:
        if (instr_is_rmw(in->op)) {
            int r = mach_operand_reg_c(&in->dst, classVregCount, cls);
            if (r >= 0) out[(*n)++] = r;
        }
        break;
    }
}

void instr_defs(const MachInstr *in, int classVregCount, RegClass cls, int out[], int *n) {
    *n = 0;
    int id = instr_def(in, classVregCount, cls);
    if (id >= 0) out[(*n)++] = id;
}

static inline void append_range(int out[], int *n, int base, int count) {
    for (int p = 0; p < count; p++) out[(*n)++] = base + p;
}

void instr_implicit_uses(const MachInstr *in, int classVregCount, RegClass cls, int out[], int *n) {
    *n = 0;
    if (cls == RC_INT) {
        switch (in->op) {
        case MACH_IDIV:
            out[(*n)++] = classVregCount + PHYS_RAX;
            out[(*n)++] = classVregCount + PHYS_RDX;
            break;
        case MACH_CQO:
            out[(*n)++] = classVregCount + PHYS_RAX;
            break;
        case MACH_CALL:
            // conservative: assume callee may read any caller-saved reg as arg
            append_range(out, n, classVregCount, PHYS_CALLER_SAVED_COUNT);
            break;
        case MACH_RET:
            out[(*n)++] = classVregCount + PHYS_RAX;
            break;
        default: break;
        }
    } else {
        switch (in->op) {
        case MACH_CALL:
            // float args already explicit MOVSS uses emitted by isel
            break;
        case MACH_RET:
            out[(*n)++] = classVregCount; /* XMM0 local color 0 */
            break;
        default: break;
        }
    }
}

void instr_implicit_defs(const MachInstr *in, int classVregCount, RegClass cls, int out[], int *n) {
    *n = 0;
    if (cls == RC_INT) {
        switch (in->op) {
        case MACH_IDIV:
            out[(*n)++] = classVregCount + PHYS_RAX;
            out[(*n)++] = classVregCount + PHYS_RDX;
            break;
        case MACH_CQO:
            out[(*n)++] = classVregCount + PHYS_RDX;
            break;
        case MACH_CALL:
            append_range(out, n, classVregCount, PHYS_CALLER_SAVED_COUNT);
            break;
        default: break;
        }
    } else {
        // whole XMM bank is caller-saved under System V ABI
        if (in->op == MACH_CALL)
            append_range(out, n, classVregCount, PHYS_XMM_COUNT);
    }
}

int instr_is_rmw(MachOpCode op) {
    switch (op) {
    case MACH_ADD: case MACH_SUB: case MACH_IMUL:
    case MACH_SAL: case MACH_NEG: case MACH_NOT: case MACH_XOR:
    case MACH_ADDSS: case MACH_SUBSS: case MACH_MULSS:
    case MACH_DIVSS: case MACH_XORPS:
        return 1;
    default: return 0;
    }
}

int instr_is_setcc(MachOpCode op) {
    switch (op) {
    case MACH_SETE: case MACH_SETNE:
    case MACH_SETL: case MACH_SETLE:
    case MACH_SETG: case MACH_SETGE:
    case MACH_SETB: case MACH_SETBE:
    case MACH_SETA: case MACH_SETAE:
        return 1;
    default: return 0;
    }
}

int instr_is_ctrl_transfer(MachOpCode op) {
    switch (op) {
    case MACH_JMP: case MACH_JE: case MACH_JNE:
    case MACH_JL:  case MACH_JLE: case MACH_JG: case MACH_JGE:
    case MACH_JB:  case MACH_JBE: case MACH_JA: case MACH_JAE:
    case MACH_CALL: case MACH_RET:
        return 1;
    default: return 0;
    }
}