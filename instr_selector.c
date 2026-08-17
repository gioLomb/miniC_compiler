/**
 * @file instr_selector.c
 * @brief Instruction selection implementation: IR → x86-64 MachInstr.
 *
 * Internal organisation
 * ----------------------
 *  1. Physical register name table  — maps MachPhysReg → AT&T name string.
 *  2. MachOperand constructors      — inline helpers: mo_vreg, mo_phys, …
 *  3. Loop-depth tracking           — g_curLoopDepth, stamped on every MachInstr.
 *  4. MachFunction helpers          — mfunc_create, mfunc_emit, mfunc_new_vreg.
 *  5. VarMap bridge                 — operand_to_vreg, load_operand, operand_to_mach.
 *  6. Comparison helpers            — flip_cmp, comparison_to_setcc, ARG_REGS table.
 *  7. PendingCmp                    — deferred comparison state for CMP+Jcc fusion.
 *  8. select_function               — main per-function selection loop.
 *  9. isel_select                   — public entry point; iterates over functions.
 * 10. isel_emit_asm                 — AT&T assembly printer.
 * 11. mach_free                     — memory cleanup.
 *
 * See instr_selector.h for the full module overview and public API documentation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "instr_selector.h"
#include "varmap.h"

/* =========================================================================
 * Physical register name table
 * =========================================================================
 * Indexed by MachPhysReg; order must match the enum exactly so that
 * phys_name64[PHYS_RAX] == "%rax", phys_name64[PHYS_AL] == "%al", etc.
 * Used by isel_emit_asm() and emit_operand() to print register names.
 * ========================================================================= */

static const char *phys_name64[] = {
    "%rax", "%rcx", "%rdx", "%rsi", "%rdi",
    "%r8",  "%r9",  "%r10", "%r11",
    "%rbx", "%r12", "%r13", "%r14", "%r15",
    "%rbp", "%rsp", "%al"
};

/* =========================================================================
 * MachOperand constructors
 * =========================================================================
 * Thin inline helpers that build a MachOperand of a specific kind.
 * Using named constructors instead of compound literals at every call site
 * makes the selection loop easier to read and avoids partially-initialised
 * union warnings from strict compilers.
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

/* =========================================================================
 * Loop-depth tracking
 * =========================================================================
 * g_curLoopDepth is updated by select_function() when processing each
 * IRInstr: it mirrors the loopDepth field stamped by the IR emitter.
 * Every MachInstr inherits this value so that the register allocator can
 * weight spill costs by 10^loopDepth — keeping hot variables in registers.
 * ========================================================================= */

static int g_curLoopDepth = 0;

/* =========================================================================
 * MachFunction helpers
 * ========================================================================= */

/**
 * @brief Allocate and initialise an empty MachFunction for function @p name.
 *
 * The instruction array starts with capacity 64 and grows geometrically
 * via realloc inside mfunc_emit().  The function name pointer is borrowed
 * from the IR and not copied; it must remain valid for the lifetime of the
 * MachFunction.
 *
 * @param name  Function name string (owned by the IR).
 * @return      Heap-allocated MachFunction; caller must free via mach_free().
 */
static MachFunction *mfunc_create(const char *name) {
    MachFunction *f = calloc(1, sizeof(MachFunction));
    f->name     = name;
    f->capacity = 64;
    f->instrs   = malloc((size_t)f->capacity * sizeof(MachInstr));
    f->nextVreg = 0;
    return f;
}

/**
 * @brief Append one MachInstr to @p f, growing the array if necessary.
 *
 * Stamps g_curLoopDepth onto the new instruction so that the register
 * allocator can use it for spill-cost weighting without re-traversing the
 * IR.  Scale is fixed at 8 (64-bit operand size for all emitted instructions).
 *
 * @param f     Target MachFunction.
 * @param op    Machine opcode.
 * @param dst   Destination operand (mo_none() if unused).
 * @param src1  First source operand.
 * @param src2  Second source operand.
 */
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

/**
 * @brief Allocate a fresh virtual register for an isel-internal temporary.
 *
 * isel-internal vregs (constants loaded into registers, intermediate results
 * of multi-instruction sequences) must not collide with the vregs assigned
 * by the VarMap pre-scan to IR operands.  select_function() initialises
 * f->nextVreg to vm.nextId after the pre-scan, so every call to this
 * function returns an id strictly above the IR-operand range.
 *
 * @param f  MachFunction whose nextVreg counter is incremented.
 * @return   Fresh vreg id, unique within this function.
 */
static inline int mfunc_new_vreg(MachFunction *f) { return f->nextVreg++; }

/* =========================================================================
 * VarMap bridge
 * =========================================================================
 * These three functions form the interface between IR Operands and the vreg
 * id space used by the machine instruction stream.
 * ========================================================================= */

/**
 * @brief Return the vreg id for IR operand @p op, or -1 if not mappable.
 *
 * Delegates to varmap_operand_id().  Returns -1 for constants, labels, and
 * function names — these are not storage locations and have no vreg.
 *
 * @param op  IR operand to translate.
 * @param vm  VarMap built during the pre-scan of this function.
 * @return    Vreg id in [0, vm.nextId), or -1.
 */
static inline int operand_to_vreg(const Operand *op, VarMap *vm) {
    return varmap_operand_id(vm, *op);
}

/**
 * @brief Load an IR operand into a vreg, emitting a MOV for constants.
 *
 * Variables and temporaries already have a vreg assigned by the VarMap
 * pre-scan; this function just returns that id.  Constants (OPND_CONST_INT,
 * OPND_CONST_FLOAT) do not have a vreg, so this function allocates a fresh
 * isel-internal vreg and emits a MOV-immediate to load the constant into it.
 *
 * Used for source operands of instructions (IDIV, STORE_ARR, array index)
 * that require all inputs to be in registers — x86-64 does not allow
 * immediate operands in all positions.
 *
 * @param op  IR source operand to load.
 * @param vm  VarMap for variable/temporary id lookup.
 * @param f   MachFunction to emit the MOV into (if needed).
 * @return    Vreg id holding the operand's value.
 */
static int load_operand(const Operand *op, VarMap *vm, MachFunction *f) {
    switch (op->kind) {
    case OPND_VAR:
    case OPND_TEMP:
        return operand_to_vreg(op, vm);
    case OPND_CONST_INT: {
        // x86-64 cannot use an immediate as a base register or IDIV operand;
        // materialise the constant in a fresh vreg first
        int dst = mfunc_new_vreg(f);
        mfunc_emit(f, MACH_MOV, mo_vreg(dst), mo_imm(op->data.intVal), mo_none());
        return dst;
    }
    case OPND_CONST_FLOAT: {
        // floats are passed as their 32-bit IEEE 754 bit pattern in an int reg
        int dst = mfunc_new_vreg(f);
        union { float fl; int i; } u; u.fl = op->data.floatVal;
        mfunc_emit(f, MACH_MOV, mo_vreg(dst), mo_imm(u.i), mo_none());
        return dst;
    }
    default: return -1;
    }
}

/**
 * @brief Convert an IR operand to a MachOperand without emitting instructions.
 *
 * Unlike load_operand(), this function never allocates vregs or emits code.
 * Constants are returned as MO_IMM inline (valid as x86-64 immediates in
 * most positions); variables and temporaries are returned as MO_VREG.
 * Used wherever an immediate is acceptable (ADD, SUB, CMP, MOV src, …).
 *
 * @param op  IR operand to convert.
 * @param vm  VarMap for variable/temporary id lookup.
 * @param mf  Unused (accepted for API uniformity with load_operand).
 * @return    Corresponding MachOperand.
 */
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
 * Comparison helpers
 * ========================================================================= */

/**
 * @brief Flip the operands of a comparison opcode (swap lhs and rhs).
 *
 * Used when the CMP instruction would need an immediate as its first operand,
 * which is not encodable on x86-64.  Swapping operands and flipping the
 * comparison preserves semantics: (imm < v) becomes (v > imm).
 *
 * @param op  Comparison IROp to flip.
 * @return    Flipped IROp (LT↔GT, LE↔GE, EQ/NE unchanged).
 */
static inline IROp flip_cmp(IROp op) {
    switch (op) {
    case IR_LT: return IR_GT; case IR_GT: return IR_LT;
    case IR_LE: return IR_GE; case IR_GE: return IR_LE;
    default:    return op;
    }
}

/**
 * @brief Map a comparison IROp to the corresponding SETcc opcode.
 *
 * Used when a comparison result must be materialised into a register
 * (i.e. the comparison is NOT fused with an immediately following IF_FALSE).
 * The SETcc instruction writes 0 or 1 into %al, which is then sign-extended
 * into the destination vreg via MOVSX.
 *
 * @param cmpOp  Comparison IROp (IR_LT, IR_LE, IR_GT, IR_GE, IR_EQ, IR_NE).
 * @return       Corresponding MachOp (MACH_SETL, MACH_SETLE, …).
 */
static inline MachOp comparison_to_setcc(IROp cmpOp) {
    switch (cmpOp) {
    case IR_LT: return MACH_SETL;  case IR_LE: return MACH_SETLE;
    case IR_GT: return MACH_SETG;  case IR_GE: return MACH_SETGE;
    case IR_EQ: return MACH_SETE;  case IR_NE: return MACH_SETNE;
    default:    return MACH_SETE;
    }
}

/**
 * @brief System V AMD64 integer argument registers, in ABI order.
 *
 * The first NUM_ARG_REGS (6) integer arguments to a function are passed in
 * these registers.  IR_PARAM instructions are lowered to MOV into ARG_REGS[i]
 * for the first six arguments, and PUSH for any excess.
 */
static const MachPhysReg ARG_REGS[] = {
    PHYS_RDI, PHYS_RSI, PHYS_RDX, PHYS_RCX, PHYS_R8, PHYS_R9
};

/* =========================================================================
 * PendingCmp — deferred comparison for CMP+Jcc fusion
 * =========================================================================
 * x86-64 decoders can fuse a CMP (or TEST) immediately followed by a Jcc
 * into a single micro-operation (macro-fusion), reducing front-end pressure.
 * To exploit this, isel defers the materialisation of comparison results:
 * when it sees IR_LT/LE/GT/GE/EQ/NE, instead of immediately emitting
 * CMP + SETcc + MOVSX, it stores the comparison in a PendingCmp struct.
 * If the very next instruction is IR_IF_FALSE on the comparison's destination,
 * the pair is lowered to CMP + Jcc (fused form).  Otherwise, the deferred
 * comparison is flushed as CMP + SETcc + MOVSX before the next instruction
 * is processed.
 * ========================================================================= */

/** State for a deferred comparison instruction awaiting possible Jcc fusion. */
typedef struct {
    int            active;   /**< 1 if a comparison is pending, 0 otherwise. */
    const IRInstr *instr;    /**< The deferred comparison IRInstr.            */
    int            dstVreg;  /**< Vreg that will hold the 0/1 result.         */
} PendingCmp;

/**
 * @brief Flush a pending comparison by materialising it as CMP + SETcc + MOVSX.
 *
 * Called when the instruction following the comparison is NOT an IF_FALSE on
 * the comparison's destination — fusion is not possible, so the full three-
 * instruction sequence must be emitted to materialise the boolean result.
 *
 * Handles the x86-64 constraint that CMP cannot take an immediate as its
 * first operand: if the lhs is an immediate and the rhs is not, operands are
 * swapped and the comparison is flipped.  If both are immediates, lhs is
 * first materialised in a fresh vreg.
 *
 * @param pcmp  Pending comparison state (cleared on return if active).
 * @param vm    VarMap for operand id lookup.
 * @param f     MachFunction to emit into.
 */
static void flush_pending_cmp(PendingCmp *pcmp, VarMap *vm, MachFunction *f) {
    if (!pcmp->active) return;

    const IRInstr *ci = pcmp->instr;
    MachOperand lhs   = operand_to_mach(&ci->src1, vm, f);
    MachOperand rhs   = operand_to_mach(&ci->src2, vm, f);
    IROp cmpOp        = ci->op;

    // x86-64 CMP: first operand cannot be an immediate; swap and flip if needed
    if (lhs.kind == MO_IMM && rhs.kind != MO_IMM) {
        MachOperand t = lhs; lhs = rhs; rhs = t;
        cmpOp = flip_cmp(cmpOp);
    }
    // both immediates: materialise lhs in a fresh vreg
    if (lhs.kind == MO_IMM) {
        int tmp = mfunc_new_vreg(f);
        mfunc_emit(f, MACH_MOV, mo_vreg(tmp), lhs, mo_none());
        lhs = mo_vreg(tmp);
    }

    // full materialisation: CMP sets flags, SETcc reads them into %al,
    // MOVSX zero-extends %al into the destination vreg
    mfunc_emit(f, MACH_CMP,                    lhs, rhs,    mo_none());
    mfunc_emit(f, comparison_to_setcc(cmpOp),  mo_phys(PHYS_AL), mo_none(), mo_none());
    mfunc_emit(f, MACH_MOVSX, mo_vreg(pcmp->dstVreg), mo_phys(PHYS_AL), mo_none());
    pcmp->active = 0;
}

/* =========================================================================
 * select_function — per-function instruction selection
 * =========================================================================
 * Strategy: VarMap pre-scan + single-pass lowering.
 *
 *  Phase 1 (pre-scan): walk every IRInstr and call varmap_operand_id() on
 *    all three operands.  This assigns a compact vreg id to every distinct
 *    IR variable and temporary.  At the end, vm.nextId == number of distinct
 *    IR storage operands.
 *
 *  Phase 2 (sync): set f->nextVreg = vm.nextId.  All subsequent calls to
 *    mfunc_new_vreg() return ids strictly above this range, so isel-internal
 *    temporaries never alias IR-operand vregs.
 *
 *  Phase 3 (selection): iterate over IRInstrs and lower each one.  The
 *    PendingCmp mechanism defers comparisons for potential CMP+Jcc fusion.
 * ========================================================================= */

/**
 * @brief Lower one IRFunction to a MachFunction.
 *
 * Performs the three-phase strategy described above.  Each IR opcode is
 * handled by a dedicated case in the main switch:
 *
 *   IR_LABEL / IR_GOTO         → MACH_LABEL / MACH_JMP
 *   IR_IF_FALSE                → CMP+Jcc (fused) or TEST+JE (generic)
 *   IR_ASSIGN                  → MOV
 *   IR_ADD / IR_SUB            → ADD / SUB with in-place optimisation
 *   IR_MUL                     → SAL (power-of-two) or IMUL
 *   IR_DIV / IR_MOD            → MOV rax,lhs; CQO; IDIV rhs; MOV dst,rax/rdx
 *   IR_NEG                     → NEG
 *   IR_NOT                     → TEST; SETE; MOVSX
 *   IR_LT/LE/GT/GE/EQ/NE      → deferred into PendingCmp
 *   IR_LOAD_ARR / IR_STORE_ARR → LOAD / STORE with SIB addressing
 *   IR_PARAM                   → buffered in param_vregs[]
 *   IR_CALL                    → flush params; MOV args; CALL; MOV dst,rax
 *   IR_RETURN                  → MOV rax,src; RET
 *
 * @param irf  IR function to lower.
 * @return     Heap-allocated MachFunction; freed by mach_free().
 */
static MachFunction *select_function(const IRFunction *irf) {
    MachFunction *f = mfunc_create(irf->name);

    // phase 1: pre-scan — assign vreg ids to all IR operands
    VarMap vm;
    varmap_init(&vm);
    for (int i = 0; i < irf->count; i++) {
        const IRInstr *in = &irf->instrs[i];
        varmap_operand_id(&vm, in->dst);
        varmap_operand_id(&vm, in->src1);
        varmap_operand_id(&vm, in->src2);
    }

    // phase 2: sync nextVreg so isel-internal temps start above IR-operand range
    f->nextVreg = vm.nextId;

    // phase 3: single-pass instruction selection
    mfunc_emit(f, MACH_FUNC_BEGIN, mo_none(), mo_none(), mo_none());

    int param_vregs[MAX_PARAMS], param_count = 0;
    PendingCmp pcmp = { .active = 0 };

    for (int i = 0; i < irf->count; i++) {
        const IRInstr *in = &irf->instrs[i];
        g_curLoopDepth = in->loopDepth;

        // flush deferred comparison if the next instruction cannot fuse with it
        if (pcmp.active) {
            int must_materialize = 1;
            if (in->op == IR_IF_FALSE)
                must_materialize = (operand_to_vreg(&in->src1, &vm) != pcmp.dstVreg);
            if (must_materialize)
                flush_pending_cmp(&pcmp, &vm, f);
        }

        switch (in->op) {

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
                // fusion path: pending CMP + this IF_FALSE → CMP + Jcc
                // the inverted Jcc is used because IF_FALSE jumps when cond == 0
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

                // invert the condition: IF_FALSE (jump when false=0) maps to
                // the negated Jcc (jump when the flag condition is NOT met)
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
                // generic path: TEST cond, cond sets ZF; JE jumps if ZF=1 (cond==0)
                mfunc_emit(f, MACH_TEST, mo_vreg(cond_vreg), mo_vreg(cond_vreg), mo_none());
                mfunc_emit(f, MACH_JE,   mo_label(lbl), mo_none(), mo_none());
            }
            break;
        }

        case IR_ASSIGN: {
            int dst         = operand_to_vreg(&in->dst, &vm);
            MachOperand src = operand_to_mach(&in->src1, &vm, f);
            mfunc_emit(f, MACH_MOV, mo_vreg(dst), src, mo_none());
            break;
        }

        case IR_ADD:
        case IR_SUB: {
            int         dst = operand_to_vreg(&in->dst,  &vm);
            MachOperand lhs = operand_to_mach(&in->src1, &vm, f);
            MachOperand rhs = operand_to_mach(&in->src2, &vm, f);
            MachOp      mop = (in->op == IR_ADD) ? MACH_ADD : MACH_SUB;
            int lhs_id      = (lhs.kind == MO_VREG) ? lhs.vregId : -1;
            int rhs_id      = (rhs.kind == MO_VREG) ? rhs.vregId : -1;

            if (lhs_id == dst) {
                // dst = dst OP rhs → in-place: OP dst, rhs (no leading MOV)
                mfunc_emit(f, mop, mo_vreg(dst), rhs, mo_none());
            } else if (in->op == IR_ADD && rhs_id == dst) {
                // dst = lhs + dst → commutative in-place: ADD dst, lhs
                mfunc_emit(f, MACH_ADD, mo_vreg(dst), lhs, mo_none());
            } else if (in->op == IR_SUB && rhs_id == dst) {
                // dst = lhs - dst → NEG dst; ADD dst, lhs
                mfunc_emit(f, MACH_NEG, mo_vreg(dst), mo_none(), mo_none());
                mfunc_emit(f, MACH_ADD, mo_vreg(dst), lhs, mo_none());
            } else {
                // general case: MOV dst, lhs; OP dst, rhs
                mfunc_emit(f, MACH_MOV, mo_vreg(dst), lhs, mo_none());
                mfunc_emit(f, mop,      mo_vreg(dst), rhs, mo_none());
            }
            break;
        }

        case IR_MUL: {
            int         dst  = operand_to_vreg(&in->dst,  &vm);
            MachOperand src1 = operand_to_mach(&in->src1, &vm, f);
            MachOperand src2 = operand_to_mach(&in->src2, &vm, f);

            // canonicalise: put the immediate (if any) on the rhs
            MachOperand reg_side = src1, imm_side = src2;
            if (src1.kind == MO_IMM && src2.kind != MO_IMM) {
                reg_side = src2; imm_side = src1;
            }

            // peephole: multiply by a power of two → left shift (latency 1 vs 3)
            if (imm_side.kind == MO_IMM && imm_side.imm > 0 &&
                (imm_side.imm & (imm_side.imm - 1)) == 0) {
                int shift = 0; long v = imm_side.imm;
                while (v > 1) { shift++; v >>= 1; }
                int reg_id = (reg_side.kind == MO_VREG) ? reg_side.vregId : -1;
                if (reg_id != dst)
                    mfunc_emit(f, MACH_MOV, mo_vreg(dst), reg_side, mo_none());
                mfunc_emit(f, MACH_SAL, mo_vreg(dst), mo_imm(shift), mo_none());
            } else {
                // general IMUL with in-place optimisation where possible
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

        case IR_DIV:
        case IR_MOD: {
            // x86-64 IDIV: dividend in RDX:RAX (CQO sign-extends RAX into RDX),
            // quotient → RAX, remainder → RDX
            int dst = operand_to_vreg(&in->dst, &vm);
            int lhs = load_operand(&in->src1, &vm, f);
            int rhs = load_operand(&in->src2, &vm, f);
            mfunc_emit(f, MACH_MOV,  mo_phys(PHYS_RAX), mo_vreg(lhs), mo_none());
            mfunc_emit(f, MACH_CQO,  mo_none(), mo_none(), mo_none());
            mfunc_emit(f, MACH_IDIV, mo_vreg(rhs), mo_none(), mo_none());
            MachPhysReg res = (in->op == IR_DIV) ? PHYS_RAX : PHYS_RDX;
            mfunc_emit(f, MACH_MOV, mo_vreg(dst), mo_phys(res), mo_none());
            break;
        }

        case IR_NEG: {
            int         dst = operand_to_vreg(&in->dst,  &vm);
            MachOperand src = operand_to_mach(&in->src1, &vm, f);
            if ((src.kind == MO_VREG ? src.vregId : -1) != dst)
                mfunc_emit(f, MACH_MOV, mo_vreg(dst), src, mo_none());
            mfunc_emit(f, MACH_NEG, mo_vreg(dst), mo_none(), mo_none());
            break;
        }

        case IR_NOT: {
            // logical NOT: TEST src,src sets ZF=1 iff src==0; SETE reads ZF into %al
            int dst = operand_to_vreg(&in->dst, &vm);
            int src = load_operand(&in->src1, &vm, f);
            mfunc_emit(f, MACH_TEST,  mo_vreg(src), mo_vreg(src), mo_none());
            mfunc_emit(f, MACH_SETE,  mo_phys(PHYS_AL), mo_none(), mo_none());
            mfunc_emit(f, MACH_MOVSX, mo_vreg(dst), mo_phys(PHYS_AL), mo_none());
            break;
        }

        case IR_LT: case IR_LE: case IR_GT: case IR_GE:
        case IR_EQ: case IR_NE: {
            // defer: if the next instruction is IF_FALSE on this vreg,
            // flush_pending_cmp is skipped and CMP+Jcc fusion happens instead
            int dst      = operand_to_vreg(&in->dst, &vm);
            pcmp.active  = 1;
            pcmp.instr   = in;
            pcmp.dstVreg = dst;
            break;
        }

        case IR_LOAD_ARR: {
            // dst = src1[src2] → LOAD dst, disp(base, index, 8)
            int dst  = operand_to_vreg(&in->dst,  &vm);
            int base = operand_to_vreg(&in->src1, &vm);
            int idx  = load_operand(&in->src2, &vm, f);
            mfunc_emit(f, MACH_LOAD, mo_vreg(dst),
                       mo_mem(base, idx, 8, 0), mo_none());
            break;
        }

        case IR_STORE_ARR: {
            // dst[src1] = src2 → STORE disp(base, index, 8), src
            int base = operand_to_vreg(&in->dst, &vm);
            int idx  = load_operand(&in->src1, &vm, f);
            int src  = load_operand(&in->src2, &vm, f);
            mfunc_emit(f, MACH_STORE,
                       mo_mem(base, idx, 8, 0), mo_vreg(src), mo_none());
            break;
        }

        case IR_PARAM: {
            // buffer the argument vreg; flushed when IR_CALL is encountered
            MachOperand src_mo = operand_to_mach(&in->src1, &vm, f);
            int src;
            if (src_mo.kind == MO_IMM) {
                // immediate: materialise in a vreg so it can be moved to an arg reg
                src = mfunc_new_vreg(f);
                mfunc_emit(f, MACH_MOV, mo_vreg(src), src_mo, mo_none());
            } else {
                src = src_mo.vregId;
            }
            if (param_count < MAX_PARAMS)
                param_vregs[param_count++] = src;
            break;
        }

        case IR_CALL: {
            int n = param_count;

            // excess arguments (beyond the first 6) go on the stack in reverse order
            for (int k = n - 1; k >= NUM_ARG_REGS; k--)
                mfunc_emit(f, MACH_PUSH, mo_vreg(param_vregs[k]),
                           mo_none(), mo_none());

            // first 6 arguments into the System V integer argument registers
            int reg_args = (n < NUM_ARG_REGS) ? n : NUM_ARG_REGS;
            for (int k = 0; k < reg_args; k++)
                mfunc_emit(f, MACH_MOV,
                           mo_phys(ARG_REGS[k]), mo_vreg(param_vregs[k]),
                           mo_none());

            mfunc_emit(f, MACH_CALL, mo_func(in->src1.data.funcName),
                       mo_none(), mo_none());

            // caller cleans up stack arguments: ADD rsp, n_excess*8
            int extra = n - NUM_ARG_REGS;
            if (extra > 0) {
                int adj = mfunc_new_vreg(f);
                mfunc_emit(f, MACH_MOV, mo_vreg(adj), mo_imm(extra * 8L), mo_none());
                mfunc_emit(f, MACH_ADD, mo_phys(PHYS_RSP), mo_vreg(adj), mo_none());
            }

            // return value is in RAX; move to the destination vreg
            int dst = operand_to_vreg(&in->dst, &vm);
            mfunc_emit(f, MACH_MOV, mo_vreg(dst), mo_phys(PHYS_RAX), mo_none());
            param_count = 0;
            break;
        }

        case IR_RETURN: {
            // return value goes in RAX per System V ABI
            int src = load_operand(&in->src1, &vm, f);
            mfunc_emit(f, MACH_MOV, mo_phys(PHYS_RAX), mo_vreg(src), mo_none());
            mfunc_emit(f, MACH_RET, mo_none(), mo_none(), mo_none());
            break;
        }

        }
    } 

    // flush any comparison that was pending at the end of the function
    // (degenerate case: comparison with no following IF_FALSE)
    flush_pending_cmp(&pcmp, &vm, f);

    // initial frame size: one 8-byte slot per vreg, rounded to 16-byte alignment
    int raw    = f->nextVreg * 8;
    f->frameSize = (raw + 15) & ~15;

    varmap_destroy(&vm);
    return f;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

/* See instr_selector.h for full documentation. */
MachProgram *isel_select(const IRProgram *ir) {
    MachProgram *mp = calloc(1, sizeof(MachProgram));
    mp->capacity  = ir->count ? ir->count : 1;
    mp->functions = malloc((size_t)mp->capacity * sizeof(MachFunction *));
    for (int i = 0; i < ir->count; i++)
        mp->functions[mp->count++] = select_function(ir->functions[i]);
    return mp;
}

/* =========================================================================
 * Assembly printer
 * ========================================================================= */

/**
 * @brief Print a single MachOperand to @p out in AT&T syntax.
 *
 * Virtual registers are printed as %vN (pre-regalloc debug form).
 * Physical registers use the 64-bit canonical name from phys_name64[].
 * Memory references are printed as disp(base,index,scale) with absent
 * components omitted.  Spill slots are printed as -N(%rbp).
 *
 * @param o    Operand to print.
 * @param out  Output stream.
 */
static void emit_operand(const MachOperand *o, FILE *out) {
    switch (o->kind) {
    case MO_NONE:  break;
    case MO_VREG:  fprintf(out, "%%v%d", o->vregId);               break;
    case MO_PHYS:  fprintf(out, "%s",    phys_name64[o->physReg]);  break;
    case MO_IMM:   fprintf(out, "$%ld",  o->imm);                  break;
    case MO_LABEL: fprintf(out, ".L%d",  o->labelId);              break;
    case MO_FUNC:  fprintf(out, "%s",    o->func);                 break;
    case MO_STACK:
        fprintf(out, "-%d(%%rbp)", o->stackOff);
        break;
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

/* See instr_selector.h for full documentation. */
void isel_emit_asm(const MachProgram *mp, FILE *out) {
    fprintf(out, "\t.text\n");
    for (int fi = 0; fi < mp->count; fi++) {
        const MachFunction *f = mp->functions[fi];
        fprintf(out, "\t.globl %s\n%s:\n", f->name, f->name);

        for (int i = 0; i < f->count; i++) {
            const MachInstr *in = &f->instrs[i];

            // instructions with special or fixed-form emission
            switch (in->op) {
            case MACH_LABEL:
                fprintf(out, ".L%d:\n", in->dst.labelId); continue;
            case MACH_FUNC_BEGIN:
                // standard function prologue
                fprintf(out, "\tpushq\t%%rbp\n");
                fprintf(out, "\tmovq\t%%rsp, %%rbp\n");
                if (f->frameSize > 0)
                    fprintf(out, "\tsubq\t$%d, %%rsp\n", f->frameSize);
                continue;
            case MACH_RET:
                // LEAVE restores RSP from RBP and pops RBP, then RET returns
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
            // SETcc always writes into %al (8-bit RAX alias)
            case MACH_SETE:  fprintf(out, "\tsete\t%%al\n");  continue;
            case MACH_SETNE: fprintf(out, "\tsetne\t%%al\n"); continue;
            case MACH_SETL:  fprintf(out, "\tsetl\t%%al\n");  continue;
            case MACH_SETLE: fprintf(out, "\tsetle\t%%al\n"); continue;
            case MACH_SETG:  fprintf(out, "\tsetg\t%%al\n");  continue;
            case MACH_SETGE: fprintf(out, "\tsetge\t%%al\n"); continue;
            // unconditional and conditional jumps
            case MACH_JMP:   fprintf(out, "\tjmp\t.L%d\n",  in->dst.labelId); continue;
            case MACH_JE:    fprintf(out, "\tje\t.L%d\n",   in->dst.labelId); continue;
            case MACH_JNE:   fprintf(out, "\tjne\t.L%d\n",  in->dst.labelId); continue;
            case MACH_JL:    fprintf(out, "\tjl\t.L%d\n",   in->dst.labelId); continue;
            case MACH_JLE:   fprintf(out, "\tjle\t.L%d\n",  in->dst.labelId); continue;
            case MACH_JG:    fprintf(out, "\tjg\t.L%d\n",   in->dst.labelId); continue;
            case MACH_JGE:   fprintf(out, "\tjge\t.L%d\n",  in->dst.labelId); continue;
            default: break;
            }

            // two-operand instructions: look up mnemonic then emit operands
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

            // AT&T memory instruction: source first, then destination
            if (in->op == MACH_LOAD || in->op == MACH_STORE) {
                emit_operand(&in->src1, out);
                fprintf(out, ", ");
                emit_operand(&in->dst, out);
            } else if (in->src2.kind != MO_NONE) {
                // three-operand form: print src1 and dst (src2 unused in AT&T two-op)
                emit_operand(&in->src1, out);
                fprintf(out, ", ");
                emit_operand(&in->dst, out);
            } else if (in->src1.kind != MO_NONE) {
                // two-operand: src1, dst
                emit_operand(&in->src1, out);
                if (in->dst.kind != MO_NONE) {
                    fprintf(out, ", ");
                    emit_operand(&in->dst, out);
                }
            } else {
                // single-operand (NEG, NOT handled above; fallback)
                emit_operand(&in->dst, out);
            }
            fprintf(out, "\n");
        }
        fprintf(out, "\n");
    }
}

/* See instr_selector.h for full documentation. */
void mach_free(MachProgram *mp) {
    if (!mp) return;
    for (int i = 0; i < mp->count; i++) {
        free(mp->functions[i]->instrs);
        free(mp->functions[i]);
    }
    free(mp->functions);
    free(mp);
}