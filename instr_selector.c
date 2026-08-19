/**
 * @file instr_selector.c
 * @brief Instruction selection implementation: IR → x86-64 MachInstr.
 *
 * Changes vs. original:
 *  - select_function() now accepts (globals, globalCount) to handle global vars.
 *  - Global scalar reads  → LEA addr + LOAD val.
 *  - Global scalar writes → LEA addr + STORE val.
 *  - Global array accesses → LEA addr (used as base in SIB).
 *  - MACH_LEA emitted as "leaq name(%rip), dst".
 *  - isel_emit_asm() accepts const IRProgram* to emit .bss/.data sections.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "instr_selector.h"
#include "varmap.h"

/* =========================================================================
 * Physical register name table
 * ========================================================================= */

static const char *phys_name64[] = {
    "%rax", "%rcx", "%rdx", "%rsi", "%rdi",
    "%r8",  "%r9",  "%r10", "%r11",
    "%rbx", "%r12", "%r13", "%r14", "%r15",
    "%rbp", "%rsp", "%al"
};

/* =========================================================================
 * MachOperand constructors
 * ========================================================================= */

static inline MachOperand mo_none(void) {
    return (MachOperand){ .kind = MO_NONE };
}
static inline MachOperand mo_vreg(int id) {
    return (MachOperand){ .kind = MO_VREG, .vregId = id };
}
static inline MachOperand mo_phys(MachPhysReg r) {
    return (MachOperand){ .kind = MO_PHYS, .physReg = (int)r };
}
static inline MachOperand mo_imm(long v) {
    return (MachOperand){ .kind = MO_IMM, .imm = v };
}
static inline MachOperand mo_label(int id) {
    return (MachOperand){ .kind = MO_LABEL, .labelId = id };
}
static inline MachOperand mo_func(const char *name) {
    return (MachOperand){ .kind = MO_FUNC, .func = name };
}
static inline MachOperand mo_mem(int base, int index, int scale, int disp) {
    return (MachOperand){ .kind = MO_MEM,
                          .mem  = { .baseVreg  = base,
                                    .indexVreg = index,
                                    .scale     = scale,
                                    .disp      = disp } };
}
static inline MachOperand mo_global(const char *name) {
    MachOperand o;
    o.kind       = MO_GLOBAL;
    o.globalName = name;
    return o;
}

/* =========================================================================
 * Loop-depth tracking
 * ========================================================================= */

static int g_curLoopDepth = 0;

/* =========================================================================
 * MachFunction helpers
 * ========================================================================= */

static MachFunction *mfunc_create(const char *name) {
    MachFunction *f = calloc(1, sizeof(MachFunction));
    f->name     = name;
    f->capacity = 64;
    f->instrs   = malloc((size_t)f->capacity * sizeof(MachInstr));
    f->nextVreg = 0;
    return f;
}

static inline void mfunc_emit(MachFunction *f, MachOp op,
                               MachOperand dst, MachOperand src1, MachOperand src2) {
    if (f->count == f->capacity) {
        f->capacity *= 2;
        f->instrs = realloc(f->instrs, (size_t)f->capacity * sizeof(MachInstr));
    }
    f->instrs[f->count++] = (MachInstr){
        .op        = op,
        .dst       = dst,
        .src1      = src1,
        .src2      = src2,
        .scale     = 8,
        .loopDepth = g_curLoopDepth,
    };
}

static inline int mfunc_new_vreg(MachFunction *f) { return f->nextVreg++; }

/* =========================================================================
 * VarMap bridge (local variables only)
 * ========================================================================= */

static inline int operand_to_vreg(const Operand *op, VarMap *vm) {
    return varmap_operand_id(vm, *op);
}

static int load_operand(const Operand *op, VarMap *vm, MachFunction *f) {
    switch (op->kind) {
    case OPND_VAR:
    case OPND_TEMP:
        return operand_to_vreg(op, vm);
    case OPND_CONST_INT: {
        int dst = mfunc_new_vreg(f);
        mfunc_emit(f, MACH_MOV, mo_vreg(dst), mo_imm(op->data.intVal), mo_none());
        return dst;
    }
    case OPND_CONST_FLOAT: {
        int dst = mfunc_new_vreg(f);
        union { float fl; int i; } u; u.fl = op->data.floatVal;
        mfunc_emit(f, MACH_MOV, mo_vreg(dst), mo_imm(u.i), mo_none());
        return dst;
    }
    default: return -1;
    }
}

static inline MachOperand operand_to_mach(const Operand *op, VarMap *vm,
                                           MachFunction *mf) {
    (void)mf;
    switch (op->kind) {
    case OPND_CONST_INT:   return mo_imm(op->data.intVal);
    case OPND_CONST_FLOAT: {
        union { float f; int i; } u; u.f = op->data.floatVal;
        return mo_imm(u.i);
    }
    case OPND_VAR:
    case OPND_TEMP:        return mo_vreg(operand_to_vreg(op, vm));
    default:               return mo_none();
    }
}

/* =========================================================================
 * Global variable helpers
 * =========================================================================
 * Globals: varLevel == 0 (global scope).
 * gaddrs[i] caches the address vreg for globals[i]; -1 = not yet emitted.
 * Address cache always valid (address of a global never changes).
 * Value cache NOT used for scalars — re-load at each use to handle writes.
 * ========================================================================= */

static inline int is_global_var(const Operand *op) {
    return op->kind == OPND_VAR && op->data.varLevel == 0;
}

/** Linear scan by symOffset → index into globals[]; returns -1 if not found. */
static int find_global_idx(int symOff,
                            const IRGlobalVar *globals, int globalCount) {
    for (int i = 0; i < globalCount; i++)
        if (globals[i].symOffset == symOff) return i;
    return -1;
}

/** Get-or-create the vreg holding the RIP-relative address of globals[idx]. */
static int get_global_addr(int idx,
                            const IRGlobalVar *globals,
                            int *gaddrs, MachFunction *f) {
    if (gaddrs[idx] >= 0) return gaddrs[idx];
    int vreg = mfunc_new_vreg(f);
    mfunc_emit(f, MACH_LEA, mo_vreg(vreg), mo_global(globals[idx].name), mo_none());
    gaddrs[idx] = vreg;
    return vreg;
}

/** Load a global scalar value into a fresh vreg. Always re-emits LOAD. */
static int load_global_scalar(int idx,
                               const IRGlobalVar *globals,
                               int *gaddrs, MachFunction *f) {
    int addr = get_global_addr(idx, globals, gaddrs, f);
    int val  = mfunc_new_vreg(f);
    mfunc_emit(f, MACH_LOAD, mo_vreg(val), mo_mem(addr, -1, 1, 0), mo_none());
    return val;
}

/**
 * Global-aware src_op: returns MO_VREG for globals (emitting LEA/LOAD as needed),
 * delegates to operand_to_mach for everything else.
 */
static MachOperand src_op(const Operand *op,
                           VarMap *vm, MachFunction *f,
                           const IRGlobalVar *globals, int globalCount, int *gaddrs) {
    if (is_global_var(op)) {
        int idx = find_global_idx(op->data.varOffset, globals, globalCount);
        if (idx >= 0) {
            int vreg = globals[idx].isArray
                       ? get_global_addr(idx, globals, gaddrs, f)
                       : load_global_scalar(idx, globals, gaddrs, f);
            return mo_vreg(vreg);
        }
    }
    return operand_to_mach(op, vm, f);
}

/**
 * Global-aware src_reg: always returns a vreg (emitting MOV for constants,
 * LEA/LOAD for globals, VarMap lookup for locals).
 */
static int src_reg(const Operand *op,
                   VarMap *vm, MachFunction *f,
                   const IRGlobalVar *globals, int globalCount, int *gaddrs) {
    if (is_global_var(op)) {
        int idx = find_global_idx(op->data.varOffset, globals, globalCount);
        if (idx >= 0)
            return globals[idx].isArray
                   ? get_global_addr(idx, globals, gaddrs, f)
                   : load_global_scalar(idx, globals, gaddrs, f);
    }
    return load_operand(op, vm, f);
}

/**
 * Return the base-address vreg for an array operand (local or global).
 * For globals: emits LEA.
 * For locals:  returns VarMap vreg (the vreg holds the frame slot for the array base).
 */
static int array_base(const Operand *op,
                      VarMap *vm, MachFunction *f,
                      const IRGlobalVar *globals, int globalCount, int *gaddrs) {
    if (is_global_var(op)) {
        int idx = find_global_idx(op->data.varOffset, globals, globalCount);
        if (idx >= 0) return get_global_addr(idx, globals, gaddrs, f);
    }
    return operand_to_vreg(op, vm);
}

/**
 * Return 1 if any source operand of a comparison instruction is a global scalar.
 * Used to decide whether to skip the deferred-CMP optimisation (flush_pending_cmp
 * re-reads raw IR operands and is not global-aware).
 */
static int cmp_has_global_scalar_src(const IRInstr *in,
                                      const IRGlobalVar *globals, int globalCount) {
    const Operand *srcs[2] = { &in->src1, &in->src2 };
    for (int s = 0; s < 2; s++) {
        if (!is_global_var(srcs[s])) continue;
        int idx = find_global_idx(srcs[s]->data.varOffset, globals, globalCount);
        if (idx >= 0 && !globals[idx].isArray) return 1;
    }
    return 0;
}

/* =========================================================================
 * Comparison helpers
 * ========================================================================= */

static inline IROp flip_cmp(IROp op) {
    switch (op) {
    case IR_LT: return IR_GT; case IR_GT: return IR_LT;
    case IR_LE: return IR_GE; case IR_GE: return IR_LE;
    default:    return op;
    }
}

static inline MachOp comparison_to_setcc(IROp cmpOp) {
    switch (cmpOp) {
    case IR_LT: return MACH_SETL;  case IR_LE: return MACH_SETLE;
    case IR_GT: return MACH_SETG;  case IR_GE: return MACH_SETGE;
    case IR_EQ: return MACH_SETE;  case IR_NE: return MACH_SETNE;
    default:    return MACH_SETE;
    }
}

static const MachPhysReg ARG_REGS[] = {
    PHYS_RDI, PHYS_RSI, PHYS_RDX, PHYS_RCX, PHYS_R8, PHYS_R9
};

/* =========================================================================
 * PendingCmp — deferred comparison for CMP+Jcc fusion
 * ========================================================================= */

typedef struct {
    int            active;
    const IRInstr *instr;
    int            dstVreg;
} PendingCmp;

static void flush_pending_cmp(PendingCmp *pcmp, VarMap *vm, MachFunction *f) {
    if (!pcmp->active) return;

    const IRInstr *ci = pcmp->instr;
    MachOperand lhs   = operand_to_mach(&ci->src1, vm, f);
    MachOperand rhs   = operand_to_mach(&ci->src2, vm, f);
    IROp cmpOp        = ci->op;

    if (lhs.kind == MO_IMM && rhs.kind != MO_IMM) {
        MachOperand t = lhs; lhs = rhs; rhs = t;
        cmpOp = flip_cmp(cmpOp);
    }
    if (lhs.kind == MO_IMM) {
        int tmp = mfunc_new_vreg(f);
        mfunc_emit(f, MACH_MOV, mo_vreg(tmp), lhs, mo_none());
        lhs = mo_vreg(tmp);
    }

    mfunc_emit(f, MACH_CMP,                    lhs, rhs,    mo_none());
    mfunc_emit(f, comparison_to_setcc(cmpOp),  mo_phys(PHYS_AL), mo_none(), mo_none());
    mfunc_emit(f, MACH_MOVSX, mo_vreg(pcmp->dstVreg), mo_phys(PHYS_AL), mo_none());
    pcmp->active = 0;
}

/* =========================================================================
 * select_function — per-function instruction selection
 * ========================================================================= */

static MachFunction *select_function(const IRFunction *irf,
                                      const IRGlobalVar *globals,
                                      int globalCount) {
    MachFunction *f = mfunc_create(irf->name);

    /* Per-global address vreg cache (-1 = LEA not yet emitted). */
    int *gaddrs = NULL;
    if (globalCount > 0) {
        gaddrs = malloc((size_t)globalCount * sizeof(int));
        for (int i = 0; i < globalCount; i++) gaddrs[i] = -1;
    }

    /* Phase 1: pre-scan — assign vreg ids to all IR operands. */
    VarMap vm;
    varmap_init(&vm);
    for (int i = 0; i < irf->count; i++) {
        const IRInstr *in = &irf->instrs[i];
        varmap_operand_id(&vm, in->dst);
        varmap_operand_id(&vm, in->src1);
        varmap_operand_id(&vm, in->src2);
    }

    /* Phase 2: sync nextVreg so isel-internal temps don't collide. */
    f->nextVreg = vm.nextId;

    /* Phase 3: single-pass instruction selection. */
    mfunc_emit(f, MACH_FUNC_BEGIN, mo_none(), mo_none(), mo_none());

    int param_vregs[MAX_PARAMS], param_count = 0;
    PendingCmp pcmp = { .active = 0 };

    for (int i = 0; i < irf->count; i++) {
        const IRInstr *in = &irf->instrs[i];
        g_curLoopDepth = in->loopDepth;

        /* Flush deferred comparison if the next instruction can't fuse. */
        if (pcmp.active) {
            int must_materialize = 1;
            if (in->op == IR_IF_FALSE)
                must_materialize = (operand_to_vreg(&in->src1, &vm) != pcmp.dstVreg);
            if (must_materialize)
                flush_pending_cmp(&pcmp, &vm, f);
        }

        switch (in->op) {

        /* ---- Control flow ---- */
        case IR_LABEL:
            mfunc_emit(f, MACH_LABEL, mo_label(in->dst.data.labelId),
                       mo_none(), mo_none());
            break;

        case IR_GOTO:
            mfunc_emit(f, MACH_JMP, mo_label(in->dst.data.labelId),
                       mo_none(), mo_none());
            break;

        case IR_IF_FALSE: {
            int cond_vreg = operand_to_vreg(&in->src1, &vm);
            int lbl       = in->dst.data.labelId;

            if (pcmp.active && cond_vreg == pcmp.dstVreg) {
                /* Fusion: pending CMP + this IF_FALSE → CMP + Jcc. */
                const IRInstr *ci = pcmp.instr;
                MachOperand lhs   = operand_to_mach(&ci->src1, &vm, f);
                MachOperand rhs   = operand_to_mach(&ci->src2, &vm, f);
                IROp cmpOp        = ci->op;

                if (lhs.kind == MO_IMM && rhs.kind != MO_IMM) {
                    MachOperand t = lhs; lhs = rhs; rhs = t;
                    cmpOp = flip_cmp(cmpOp);
                }
                if (lhs.kind == MO_IMM) {
                    int tmp = mfunc_new_vreg(f);
                    mfunc_emit(f, MACH_MOV, mo_vreg(tmp), lhs, mo_none());
                    lhs = mo_vreg(tmp);
                }
                mfunc_emit(f, MACH_CMP, lhs, rhs, mo_none());

                MachOp jcc;
                switch (cmpOp) {
                case IR_LT: jcc = MACH_JGE; break; case IR_LE: jcc = MACH_JG;  break;
                case IR_GT: jcc = MACH_JLE; break; case IR_GE: jcc = MACH_JL;  break;
                case IR_EQ: jcc = MACH_JNE; break; case IR_NE: jcc = MACH_JE;  break;
                default:    jcc = MACH_JMP; break;
                }
                mfunc_emit(f, jcc, mo_label(lbl), mo_none(), mo_none());
                pcmp.active = 0;
            } else {
                mfunc_emit(f, MACH_TEST, mo_vreg(cond_vreg), mo_vreg(cond_vreg), mo_none());
                mfunc_emit(f, MACH_JE,   mo_label(lbl), mo_none(), mo_none());
            }
            break;
        }

        /* ---- Assignment ---- */
        case IR_ASSIGN: {
            /* Write to global scalar → STORE via RIP-relative address. */
            if (is_global_var(&in->dst)) {
                int idx = find_global_idx(in->dst.data.varOffset, globals, globalCount);
                if (idx >= 0 && !globals[idx].isArray) {
                    MachOperand sv = src_op(&in->src1, &vm, f, globals, globalCount, gaddrs);
                    int val_vreg;
                    if (sv.kind == MO_VREG) {
                        val_vreg = sv.vregId;
                    } else {
                        val_vreg = mfunc_new_vreg(f);
                        mfunc_emit(f, MACH_MOV, mo_vreg(val_vreg), sv, mo_none());
                    }
                    int addr = get_global_addr(idx, globals, gaddrs, f);
                    mfunc_emit(f, MACH_STORE,
                               mo_mem(addr, -1, 1, 0), mo_vreg(val_vreg), mo_none());
                    break;
                }
            }
            /* Normal (local) assignment. */
            int         dst = operand_to_vreg(&in->dst, &vm);
            MachOperand src = src_op(&in->src1, &vm, f, globals, globalCount, gaddrs);
            mfunc_emit(f, MACH_MOV, mo_vreg(dst), src, mo_none());
            break;
        }

        /* ---- Arithmetic: ADD / SUB ---- */
        case IR_ADD:
        case IR_SUB: {
            int         dst = operand_to_vreg(&in->dst,  &vm);
            MachOperand lhs = src_op(&in->src1, &vm, f, globals, globalCount, gaddrs);
            MachOperand rhs = src_op(&in->src2, &vm, f, globals, globalCount, gaddrs);
            MachOp      mop = (in->op == IR_ADD) ? MACH_ADD : MACH_SUB;
            int lhs_id = (lhs.kind == MO_VREG) ? lhs.vregId : -1;
            int rhs_id = (rhs.kind == MO_VREG) ? rhs.vregId : -1;

            if (lhs_id == dst) {
                mfunc_emit(f, mop, mo_vreg(dst), rhs, mo_none());
            } else if (in->op == IR_ADD && rhs_id == dst) {
                mfunc_emit(f, MACH_ADD, mo_vreg(dst), lhs, mo_none());
            } else if (in->op == IR_SUB && rhs_id == dst) {
                mfunc_emit(f, MACH_NEG, mo_vreg(dst), mo_none(), mo_none());
                mfunc_emit(f, MACH_ADD, mo_vreg(dst), lhs, mo_none());
            } else {
                mfunc_emit(f, MACH_MOV, mo_vreg(dst), lhs, mo_none());
                mfunc_emit(f, mop,      mo_vreg(dst), rhs, mo_none());
            }
            break;
        }

        /* ---- Arithmetic: MUL ---- */
        case IR_MUL: {
            int         dst  = operand_to_vreg(&in->dst,  &vm);
            MachOperand src1 = src_op(&in->src1, &vm, f, globals, globalCount, gaddrs);
            MachOperand src2 = src_op(&in->src2, &vm, f, globals, globalCount, gaddrs);

            MachOperand reg_side = src1, imm_side = src2;
            if (src1.kind == MO_IMM && src2.kind != MO_IMM) {
                reg_side = src2; imm_side = src1;
            }

            if (imm_side.kind == MO_IMM && imm_side.imm > 0 &&
                (imm_side.imm & (imm_side.imm - 1)) == 0) {
                /* Power-of-two → left shift. */
                int shift = 0; long v = imm_side.imm;
                while (v > 1) { shift++; v >>= 1; }
                int reg_id = (reg_side.kind == MO_VREG) ? reg_side.vregId : -1;
                if (reg_id != dst)
                    mfunc_emit(f, MACH_MOV, mo_vreg(dst), reg_side, mo_none());
                mfunc_emit(f, MACH_SAL, mo_vreg(dst), mo_imm(shift), mo_none());
            } else {
                int src1_id = (src1.kind == MO_VREG) ? src1.vregId : -1;
                int src2_id = (src2.kind == MO_VREG) ? src2.vregId : -1;
                if (src1_id == dst) {
                    mfunc_emit(f, MACH_IMUL, mo_vreg(dst), src2, mo_none());
                } else if (src2_id == dst) {
                    mfunc_emit(f, MACH_IMUL, mo_vreg(dst), src1, mo_none());
                } else {
                    mfunc_emit(f, MACH_MOV,  mo_vreg(dst), src1, mo_none());
                    mfunc_emit(f, MACH_IMUL, mo_vreg(dst), src2, mo_none());
                }
            }
            break;
        }

        /* ---- Arithmetic: DIV / MOD ---- */
        case IR_DIV:
        case IR_MOD: {
            int dst = operand_to_vreg(&in->dst, &vm);
            int lhs = src_reg(&in->src1, &vm, f, globals, globalCount, gaddrs);
            int rhs = src_reg(&in->src2, &vm, f, globals, globalCount, gaddrs);
            mfunc_emit(f, MACH_MOV,  mo_phys(PHYS_RAX), mo_vreg(lhs), mo_none());
            mfunc_emit(f, MACH_CQO,  mo_none(), mo_none(), mo_none());
            mfunc_emit(f, MACH_IDIV, mo_vreg(rhs), mo_none(), mo_none());
            MachPhysReg res = (in->op == IR_DIV) ? PHYS_RAX : PHYS_RDX;
            mfunc_emit(f, MACH_MOV, mo_vreg(dst), mo_phys(res), mo_none());
            break;
        }

        /* ---- Unary: NEG ---- */
        case IR_NEG: {
            int         dst = operand_to_vreg(&in->dst,  &vm);
            MachOperand src = src_op(&in->src1, &vm, f, globals, globalCount, gaddrs);
            if ((src.kind == MO_VREG ? src.vregId : -1) != dst)
                mfunc_emit(f, MACH_MOV, mo_vreg(dst), src, mo_none());
            mfunc_emit(f, MACH_NEG, mo_vreg(dst), mo_none(), mo_none());
            break;
        }

        /* ---- Unary: NOT ---- */
        case IR_NOT: {
            int dst = operand_to_vreg(&in->dst, &vm);
            int sv  = src_reg(&in->src1, &vm, f, globals, globalCount, gaddrs);
            mfunc_emit(f, MACH_TEST,  mo_vreg(sv),  mo_vreg(sv),  mo_none());
            mfunc_emit(f, MACH_SETE,  mo_phys(PHYS_AL), mo_none(), mo_none());
            mfunc_emit(f, MACH_MOVSX, mo_vreg(dst), mo_phys(PHYS_AL), mo_none());
            break;
        }

        /* ---- Comparisons ---- */
        case IR_LT: case IR_LE: case IR_GT: case IR_GE:
        case IR_EQ: case IR_NE: {
            int dst = operand_to_vreg(&in->dst, &vm);

            /* If any source is a global scalar, flush_pending_cmp would
             * re-read raw IR operands (not global-aware) — emit immediately. */
            if (cmp_has_global_scalar_src(in, globals, globalCount)) {
                MachOperand lhs = src_op(&in->src1, &vm, f, globals, globalCount, gaddrs);
                MachOperand rhs = src_op(&in->src2, &vm, f, globals, globalCount, gaddrs);
                IROp cmpOp = in->op;
                if (lhs.kind == MO_IMM && rhs.kind != MO_IMM) {
                    MachOperand t = lhs; lhs = rhs; rhs = t; cmpOp = flip_cmp(cmpOp);
                }
                if (lhs.kind == MO_IMM) {
                    int tmp = mfunc_new_vreg(f);
                    mfunc_emit(f, MACH_MOV, mo_vreg(tmp), lhs, mo_none());
                    lhs = mo_vreg(tmp);
                }
                mfunc_emit(f, MACH_CMP, lhs, rhs, mo_none());
                mfunc_emit(f, comparison_to_setcc(cmpOp), mo_phys(PHYS_AL), mo_none(), mo_none());
                mfunc_emit(f, MACH_MOVSX, mo_vreg(dst), mo_phys(PHYS_AL), mo_none());
            } else {
                pcmp.active  = 1;
                pcmp.instr   = in;
                pcmp.dstVreg = dst;
            }
            break;
        }

        /* ---- Array load ---- */
        case IR_LOAD_ARR: {
            int dst  = operand_to_vreg(&in->dst,  &vm);
            int base = array_base(&in->src1, &vm, f, globals, globalCount, gaddrs);
            int idx  = src_reg(&in->src2, &vm, f, globals, globalCount, gaddrs);
            mfunc_emit(f, MACH_LOAD, mo_vreg(dst),
                       mo_mem(base, idx, 8, 0), mo_none());
            break;
        }

        /* ---- Array store ---- */
        case IR_STORE_ARR: {
            int base = array_base(&in->dst,  &vm, f, globals, globalCount, gaddrs);
            int idx  = src_reg(&in->src1, &vm, f, globals, globalCount, gaddrs);
            int src  = src_reg(&in->src2, &vm, f, globals, globalCount, gaddrs);
            mfunc_emit(f, MACH_STORE,
                       mo_mem(base, idx, 8, 0), mo_vreg(src), mo_none());
            break;
        }

        /* ---- Function call arguments ---- */
        case IR_PARAM: {
            MachOperand sv = src_op(&in->src1, &vm, f, globals, globalCount, gaddrs);
            int sv_vreg;
            if (sv.kind == MO_IMM) {
                sv_vreg = mfunc_new_vreg(f);
                mfunc_emit(f, MACH_MOV, mo_vreg(sv_vreg), sv, mo_none());
            } else {
                sv_vreg = sv.vregId;
            }
            if (param_count < MAX_PARAMS)
                param_vregs[param_count++] = sv_vreg;
            break;
        }

        /* ---- Function call ---- */
        case IR_CALL: {
            int n = param_count;
            for (int k = n - 1; k >= NUM_ARG_REGS; k--)
                mfunc_emit(f, MACH_PUSH, mo_vreg(param_vregs[k]),
                           mo_none(), mo_none());
            int reg_args = (n < NUM_ARG_REGS) ? n : NUM_ARG_REGS;
            for (int k = 0; k < reg_args; k++)
                mfunc_emit(f, MACH_MOV,
                           mo_phys(ARG_REGS[k]), mo_vreg(param_vregs[k]),
                           mo_none());
            mfunc_emit(f, MACH_CALL, mo_func(in->src1.data.funcName),
                       mo_none(), mo_none());
            int extra = n - NUM_ARG_REGS;
            if (extra > 0) {
                int adj = mfunc_new_vreg(f);
                mfunc_emit(f, MACH_MOV, mo_vreg(adj), mo_imm(extra * 8L), mo_none());
                mfunc_emit(f, MACH_ADD, mo_phys(PHYS_RSP), mo_vreg(adj), mo_none());
            }
            int dst = operand_to_vreg(&in->dst, &vm);
            mfunc_emit(f, MACH_MOV, mo_vreg(dst), mo_phys(PHYS_RAX), mo_none());
            param_count = 0;
            break;
        }

        /* ---- Return ---- */
        case IR_RETURN: {
            int sv = src_reg(&in->src1, &vm, f, globals, globalCount, gaddrs);
            mfunc_emit(f, MACH_MOV, mo_phys(PHYS_RAX), mo_vreg(sv), mo_none());
            mfunc_emit(f, MACH_RET, mo_none(), mo_none(), mo_none());
            break;
        }

        } /* switch */
    } /* for each IR instruction */

    flush_pending_cmp(&pcmp, &vm, f);

    int raw      = f->nextVreg * 8;
    f->frameSize = (raw + 15) & ~15;

    free(gaddrs);
    varmap_destroy(&vm);
    return f;
}

/* =========================================================================
 * Public API — isel_select
 * ========================================================================= */

MachProgram *isel_select(const IRProgram *ir) {
    MachProgram *mp = calloc(1, sizeof(MachProgram));
    mp->capacity    = ir->count ? ir->count : 1;
    mp->functions   = malloc((size_t)mp->capacity * sizeof(MachFunction *));
    for (int i = 0; i < ir->count; i++)
        mp->functions[mp->count++] = select_function(ir->functions[i],
                                                       ir->globals,
                                                       ir->globalCount);
    return mp;
}

/* =========================================================================
 * Assembly printer
 * ========================================================================= */

static void emit_operand(const MachOperand *o, FILE *out) {
    switch (o->kind) {
    case MO_NONE:   break;
    case MO_VREG:   fprintf(out, "%%v%d",      o->vregId);               break;
    case MO_PHYS:   fprintf(out, "%s",          phys_name64[o->physReg]); break;
    case MO_IMM:    fprintf(out, "$%ld",        o->imm);                  break;
    case MO_LABEL:  fprintf(out, ".L%d",        o->labelId);              break;
    case MO_FUNC:   fprintf(out, "%s",          o->func);                 break;
    case MO_GLOBAL: fprintf(out, "%s(%%rip)",   o->globalName);           break;
    case MO_STACK:  fprintf(out, "-%d(%%rbp)",  o->stackOff);             break;
    case MO_MEM:
        if (o->mem.disp) fprintf(out, "%d", o->mem.disp);
        fprintf(out, "(");
        if (o->mem.baseVreg  >= 0) fprintf(out, "%s", phys_name64[o->mem.baseVreg]);
        if (o->mem.indexVreg >= 0) fprintf(out, ",%s,%d",
                                            phys_name64[o->mem.indexVreg],
                                            o->mem.scale);
        fprintf(out, ")");
        break;
    case MO_FIMM:
        fprintf(out, "$0x%x", (unsigned)(int)o->fimm);
        break;
    }
}

/* =========================================================================
 * Public API — isel_emit_asm
 * ========================================================================= */

void isel_emit_asm(const MachProgram *mp, const IRProgram *ir, FILE *out) {

    /* --- Global variable declarations --- */
    if (ir && ir->globalCount > 0) {
        /* .bss: zero-initialised (no explicit init or all-zero). */
        int has_bss = 0;
        for (int i = 0; i < ir->globalCount; i++)
            if (ir->globals[i].initCount == 0) { has_bss = 1; break; }

        if (has_bss) {
            fprintf(out, "\t.bss\n");
            for (int i = 0; i < ir->globalCount; i++) {
                const IRGlobalVar *g = &ir->globals[i];
                if (g->initCount > 0) continue;
                int nelems = g->isArray ? g->arraySize : 1;
                fprintf(out, "\t.globl %s\n%s:\n", g->name, g->name);
                fprintf(out, "\t.zero %d\n", nelems * 8);
            }
        }

        /* .data: explicitly initialised globals. */
        int has_data = 0;
        for (int i = 0; i < ir->globalCount; i++)
            if (ir->globals[i].initCount > 0) { has_data = 1; break; }

        if (has_data) {
            fprintf(out, "\t.data\n");
            for (int i = 0; i < ir->globalCount; i++) {
                const IRGlobalVar *g = &ir->globals[i];
                if (g->initCount == 0) continue;
                int nelems = g->isArray ? g->arraySize : 1;
                fprintf(out, "\t.globl %s\n%s:\n", g->name, g->name);
                for (int j = 0; j < nelems; j++) {
                    long val = (j < g->initCount) ? g->initVals[j] : 0L;
                    fprintf(out, "\t.quad %ld\n", val);
                }
            }
        }
    }

    /* --- Code section --- */
    fprintf(out, "\t.text\n");
    for (int fi = 0; fi < mp->count; fi++) {
        const MachFunction *f = mp->functions[fi];
        fprintf(out, "\t.globl %s\n%s:\n", f->name, f->name);

        for (int i = 0; i < f->count; i++) {
            const MachInstr *in = &f->instrs[i];

            switch (in->op) {
            case MACH_LABEL:
                fprintf(out, ".L%d:\n", in->dst.labelId); continue;
            case MACH_FUNC_BEGIN:
                fprintf(out, "\tpushq\t%%rbp\n");
                fprintf(out, "\tmovq\t%%rsp, %%rbp\n");
                if (f->frameSize > 0)
                    fprintf(out, "\tsubq\t$%d, %%rsp\n", f->frameSize);
                continue;
            case MACH_RET:
                fprintf(out, "\tleave\n\tret\n"); continue;
            case MACH_CQO:
                fprintf(out, "\tcqo\n"); continue;
            case MACH_IDIV:
                fprintf(out, "\tidivq\t"); emit_operand(&in->dst, out);
                fprintf(out, "\n"); continue;
            case MACH_NEG:
                fprintf(out, "\tnegq\t"); emit_operand(&in->dst, out);
                fprintf(out, "\n"); continue;
            case MACH_NOT:
                fprintf(out, "\tnotq\t"); emit_operand(&in->dst, out);
                fprintf(out, "\n"); continue;
            case MACH_PUSH:
                fprintf(out, "\tpushq\t"); emit_operand(&in->dst, out);
                fprintf(out, "\n"); continue;
            case MACH_POP:
                fprintf(out, "\tpopq\t"); emit_operand(&in->dst, out);
                fprintf(out, "\n"); continue;
            case MACH_CALL:
                fprintf(out, "\tcall\t"); emit_operand(&in->dst, out);
                fprintf(out, "\n"); continue;
            case MACH_LEA:
                /* leaq globalname(%rip), %dst */
                fprintf(out, "\tleaq\t");
                emit_operand(&in->src1, out);
                fprintf(out, ", ");
                emit_operand(&in->dst, out);
                fprintf(out, "\n"); continue;
            case MACH_SETE:  fprintf(out, "\tsete\t%%al\n");  continue;
            case MACH_SETNE: fprintf(out, "\tsetne\t%%al\n"); continue;
            case MACH_SETL:  fprintf(out, "\tsetl\t%%al\n");  continue;
            case MACH_SETLE: fprintf(out, "\tsetle\t%%al\n"); continue;
            case MACH_SETG:  fprintf(out, "\tsetg\t%%al\n");  continue;
            case MACH_SETGE: fprintf(out, "\tsetge\t%%al\n"); continue;
            case MACH_JMP:   fprintf(out, "\tjmp\t.L%d\n",  in->dst.labelId); continue;
            case MACH_JE:    fprintf(out, "\tje\t.L%d\n",   in->dst.labelId); continue;
            case MACH_JNE:   fprintf(out, "\tjne\t.L%d\n",  in->dst.labelId); continue;
            case MACH_JL:    fprintf(out, "\tjl\t.L%d\n",   in->dst.labelId); continue;
            case MACH_JLE:   fprintf(out, "\tjle\t.L%d\n",  in->dst.labelId); continue;
            case MACH_JG:    fprintf(out, "\tjg\t.L%d\n",   in->dst.labelId); continue;
            case MACH_JGE:   fprintf(out, "\tjge\t.L%d\n",  in->dst.labelId); continue;
            default: break;
            }

            /* Two-operand instructions. */
            const char *mnem = NULL;
            switch (in->op) {
            case MACH_MOV:   mnem = "movq";   break;
            case MACH_MOVSX: mnem = "movsbq"; break;
            case MACH_ADD:   mnem = "addq";   break;
            case MACH_SUB:   mnem = "subq";   break;
            case MACH_IMUL:  mnem = "imulq";  break;
            case MACH_SAL:   mnem = "salq";   break;
            case MACH_XOR:   mnem = "xorq";   break;
            case MACH_CMP:   mnem = "cmpq";   break;
            case MACH_TEST:  mnem = "testq";  break;
            case MACH_LOAD:  mnem = "movq";   break;
            case MACH_STORE: mnem = "movq";   break;
            default:         mnem = "???";    break;
            }

            fprintf(out, "\t%s\t", mnem);

            if (in->op == MACH_LOAD || in->op == MACH_STORE) {
                emit_operand(&in->src1, out);
                fprintf(out, ", ");
                emit_operand(&in->dst, out);
            } else if (in->src2.kind != MO_NONE) {
                emit_operand(&in->src1, out);
                fprintf(out, ", ");
                emit_operand(&in->dst, out);
            } else if (in->src1.kind != MO_NONE) {
                emit_operand(&in->src1, out);
                if (in->dst.kind != MO_NONE) {
                    fprintf(out, ", ");
                    emit_operand(&in->dst, out);
                }
            } else {
                emit_operand(&in->dst, out);
            }
            fprintf(out, "\n");
        }
        fprintf(out, "\n");
    }
}

/* =========================================================================
 * Public API — mach_free
 * ========================================================================= */

void mach_free(MachProgram *mp) {
    if (!mp) return;
    for (int i = 0; i < mp->count; i++) {
        free(mp->functions[i]->instrs);
        free(mp->functions[i]);
    }
    free(mp->functions);
    free(mp);
}