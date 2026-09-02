/**
 * @file instr_selector.c
 * @brief Instruction selection: IR -> x86-64 MachInstr.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "instr_selector.h"
#include "varmap.h"

// indexed by MachPhysReg enum value; last entry ("%al") is the 8-bit
// alias of PHYS_RAX, used only when printing SETcc destinations
static const char *phys_name64[] = {
    "%rax", "%rcx", "%rdx", "%rsi", "%rdi",
    "%r8",  "%r9",  "%r10", "%r11",
    "%rbx", "%r12", "%r13", "%r14", "%r15",
    "%rbp", "%rsp", "%al"
};

// System V AMD64 ABI: first 6 integer/pointer arguments go in these
// registers, in this exact order; anything beyond NUM_ARG_REGS is spilled
// to the stack by the caller (see IR_CALL / IR_PARAM handling below)
static const MachPhysReg ARG_REGS[] = {
    PHYS_RDI, PHYS_RSI, PHYS_RDX, PHYS_RCX, PHYS_R8, PHYS_R9
};

/**
 * @brief Staging buffer for IR_PARAM arguments awaiting the next IR_CALL.
 *
 * IR_PARAM instructions arrive one at a time, in order, before the IR_CALL
 * that consumes them; the actual register/stack placement only happens
 * once IR_CALL is reached (arity is only known then).
 */
typedef struct {
    int vregs[MAX_PARAMS];
    int count;
} PendingArgs;

/**
 * @brief State for a comparison instruction whose CMP/SETcc emission is
 *        deferred until we know how it will be consumed.
 *
 * IR comparisons (IR_LT, IR_EQ, ...) normally materialise a 0/1 value via
 * CMP + SETcc + MOVSX. But if the very next IR instruction is an
 * IR_IF_FALSE testing that same result, the value never needs to exist as
 * a byte in memory/register at all — a single CMP + Jcc suffices. Deferring
 * the emission lets select_function() choose the cheaper path once it sees
 * how the comparison result is used.
 */
typedef struct {
    int            active;   /**< 1 if a comparison is currently pending materialisation. */
    const IRInstr *instr;    /**< The IR_LT/IR_EQ/... instruction being deferred. */
    int            dstVreg;  /**< vreg that would hold the materialised 0/1 result. */
} PendingCmp;


// mirrors IRInstr.loopDepth: copied onto every MachInstr emitted so the
// register allocator's spill-cost heuristic (10^loopDepth) survives
// lowering from IR to machine code
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

static inline void mfunc_emit(MachFunction *f, MachOpCode op,
                               MachOperand dst, MachOperand src1, MachOperand src2) {
    // standard doubling growth for the instruction array
    if (f->count == f->capacity) {
        f->capacity *= 2;
        f->instrs = realloc(f->instrs, (size_t)f->capacity * sizeof(MachInstr));
    }
    f->instrs[f->count++] = (MachInstr){
        .op        = op,
        .dst       = dst,
        .src1      = src1,
        .src2      = src2,
        .scale     = 8,               // default element size for future MO_MEM use
        .loopDepth = g_curLoopDepth,  // stamp current static loop nesting
    };
}

static inline int mfunc_new_vreg(MachFunction *f) { return f->nextVreg++; }

/**
 * @brief Materialise an IR operand into a vreg, emitting a load if it is
 *        a literal constant.
 *
 * Unlike operand_to_mach(), this ALWAYS returns a vreg id (never an
 * immediate MachOperand). Used where the target machine instruction has
 * no immediate-operand form (e.g. IDIV divisor, PUSH source) and the
 * value must sit in a register regardless of how it was expressed in IR.
 *
 * @param op            IR operand to materialise.
 * @param operandToVreg VarMap providing the operand -> vreg id mapping.
 * @param f             Machine function new MOVs (for constants) are emitted into.
 */
static int load_operand(const Operand *op, VarMap *operandToVreg, MachFunction *f) {
    switch (op->kind) {
    case OPND_VAR:
    case OPND_TEMP:
        // already a storage location: just resolve its vreg id
        return varmap_operand_id(operandToVreg, *op);
    case OPND_CONST_INT: {
        // materialise the constant with a MOV into a fresh vreg
        int dst = mfunc_new_vreg(f);
        mfunc_emit(f, MACH_MOV, (MachOperand){ .kind = MO_VREG, .vregId = dst }, (MachOperand){ .kind = MO_IMM, .imm = op->data.intVal }, (MachOperand){ .kind = MO_NONE });
        return dst;
    }
    case OPND_CONST_FLOAT: {
        // float constants are moved as their raw bit pattern (no SSE support
        // in this backend); reinterpret via union to avoid UB from casting
        // float->long directly, which would convert the VALUE not the bits
        int dst = mfunc_new_vreg(f);
        union { float fl; int i; } u; u.fl = op->data.floatVal;
        mfunc_emit(f, MACH_MOV, (MachOperand){ .kind = MO_VREG, .vregId = dst }, (MachOperand){ .kind = MO_IMM, .imm = u.i }, (MachOperand){ .kind = MO_NONE });
        return dst;
    }
    default: return -1; // labels/functions/none: not a loadable value
    }
}

/**
 * @brief Convert an IR operand directly to a MachOperand, WITHOUT forcing
 *        constants into a register.
 *
 * Used wherever the target instruction accepts an immediate operand
 * (ADD/SUB/CMP/MOV second operand, etc.), avoiding a redundant MOV that
 * load_operand() would otherwise emit.
 *
 * @param op            IR operand to convert.
 * @param operandToVreg VarMap providing the operand -> vreg id mapping.
 * @param mf            Unused (kept for call-site symmetry with load_operand()).
 */
static inline MachOperand operand_to_mach(const Operand *op, VarMap *operandToVreg,
                                           MachFunction *mf) {
    (void)mf;
    switch (op->kind) {
    case OPND_CONST_INT:   return (MachOperand){ .kind = MO_IMM, .imm = op->data.intVal };
    case OPND_CONST_FLOAT: {
        // same bit-reinterpretation trick as load_operand()
        union { float f; int i; } u; u.f = op->data.floatVal;
        return (MachOperand){ .kind = MO_IMM, .imm = u.i };
    }
    case OPND_VAR:
    case OPND_TEMP:        return (MachOperand){ .kind = MO_VREG, .vregId = varmap_operand_id(operandToVreg, *op) };
    default:               return (MachOperand){ .kind = MO_NONE };
    }
}


// linear scan: globalCount is small (typically a handful of top-level
// declarations), so an O(1) map would be overkill here
static int find_global_idx(int symOff,
                            const IRGlobalVar *globals, int globalCount) {
    for (int i = 0; i < globalCount; i++)
        if (globals[i].symOffset == symOff) return i;
    return -1;
}

/* =========================================================================
 * Comparison helpers
 * ========================================================================= */

/**
 * @brief Swap the sense of a relational operator (a OP b  ->  b OP' a).
 *
 * Needed when the CMP's left-hand operand turns out to be an immediate
 * (x86 CMP cannot take an immediate as its first/destination-like operand
 * in this backend's encoding), so the operands are swapped and the
 * comparison flipped to preserve the original truth value.
 */
static inline IROp flip_cmp(IROp op) {
    switch (op) {
    case IR_LT: return IR_GT; case IR_GT: return IR_LT;
    case IR_LE: return IR_GE; case IR_GE: return IR_LE;
    default:    return op; // EQ/NE are symmetric: no flip needed
    }
}

// maps an IR comparison opcode to the x86 SETcc variant that materialises
// its boolean result into %al
static inline MachOpCode comparison_to_setcc(IROp cmpOp) {
    switch (cmpOp) {
    case IR_LT: return MACH_SETL;  case IR_LE: return MACH_SETLE;
    case IR_GT: return MACH_SETG;  case IR_GE: return MACH_SETGE;
    case IR_EQ: return MACH_SETE;  case IR_NE: return MACH_SETNE;
    default:    return MACH_SETE;
    }
}


/**
 * @brief Force-materialise a pending comparison as CMP + SETcc + MOVSX.
 *
 * Called whenever the next instruction turns out NOT to be a matching
 * IR_IF_FALSE (so fusion is not possible) — the boolean value must be
 * computed the "normal" way after all.
 */
static void flush_pending_cmp(PendingCmp *pcmp, VarMap *operandToVreg, MachFunction *f) {
    if (!pcmp->active) return;

    const IRInstr *ci = pcmp->instr;
    MachOperand lhs   = operand_to_mach(&ci->src1, operandToVreg, f);
    MachOperand rhs   = operand_to_mach(&ci->src2, operandToVreg, f);
    IROp cmpOp        = ci->op;

    // x86 CMP requires the first operand to be a register; if the IR gave
    // us an immediate on the left, swap operands and flip the comparison
    // sense so the encoded instruction stays valid and semantically equal
    if (lhs.kind == MO_IMM && rhs.kind != MO_IMM) {
        MachOperand t = lhs; lhs = rhs; rhs = t;
        cmpOp = flip_cmp(cmpOp);
    }
    // both operands immediate: materialise the left one into a register so CMP has a valid encoding
    if (lhs.kind == MO_IMM) {
        int tmp = mfunc_new_vreg(f);
        mfunc_emit(f, MACH_MOV, (MachOperand){ .kind = MO_VREG, .vregId = tmp }, lhs, (MachOperand){ .kind = MO_NONE });
        lhs = (MachOperand){ .kind = MO_VREG, .vregId = tmp };
    }

    // CMP sets flags; SETcc reads flags into %al; MOVSX sign-extends the
    // 1-byte 0/1 result into the full 64-bit destination vreg
    mfunc_emit(f, MACH_CMP,                    lhs, rhs,    (MachOperand){ .kind = MO_NONE });
    mfunc_emit(f, comparison_to_setcc(cmpOp),  (MachOperand){ .kind = MO_PHYS, .physReg = PHYS_AL }, (MachOperand){ .kind = MO_NONE }, (MachOperand){ .kind = MO_NONE });
    mfunc_emit(f, MACH_MOVSX, (MachOperand){ .kind = MO_VREG, .vregId = pcmp->dstVreg }, (MachOperand){ .kind = MO_PHYS, .physReg = PHYS_AL }, (MachOperand){ .kind = MO_NONE });
    pcmp->active = 0;
}

/* =========================================================================
 * Per-function setup helpers
 * ========================================================================= */

/**
 * @brief Phase 1+2 of select_function: register every IR operand (including
 *        unused formal parameters) with a stable vreg id, then sync
 *        f->nextVreg so isel-internal temporaries never collide with them.
 */
static void select_prescan_and_sync_vregs(const IRFunction *irf, VarMap *operandToVreg,
                                           MachFunction *f) {
    // walk every instruction once just to register every distinct
    // variable/temp with a stable vreg id BEFORE any code is emitted
    for (int i = 0; i < irf->count; i++) {
        const IRInstr *in = &irf->instrs[i];
        varmap_operand_id(operandToVreg, in->dst);
        varmap_operand_id(operandToVreg, in->src1);
        varmap_operand_id(operandToVreg, in->src2);
    }
    // also register formal parameters even if unused in the body, so their
    // ids stay dense and don't get silently skipped
    for (int p = 0; p < irf->paramCount; p++)
        varmap_operand_id(operandToVreg, irf->params[p]);

    // any fresh vreg allocated later (mfunc_new_vreg) must start counting
    // AFTER every id already handed out by VarMap above
    f->nextVreg = operandToVreg->nextId;
}

/**
 * @brief Phase 3-bis of select_function: bind formal parameters to their
 *        ABI registers (System V: rdi,rsi,rdx,rcx,r8,r9).
 *
 * Parameters beyond the 6th (passed on the caller's stack) are not yet
 * supported — flagged with a diagnostic rather than silently miscompiled.
 */
static void select_bind_params(const IRFunction *irf, VarMap *operandToVreg, MachFunction *f) {
    for (int p = 0; p < irf->paramCount; p++) {
        int vreg = varmap_operand_id(operandToVreg, irf->params[p]);
        if (p < NUM_ARG_REGS) {
            mfunc_emit(f, MACH_MOV, (MachOperand){ .kind = MO_VREG, .vregId = vreg }, (MachOperand){ .kind = MO_PHYS, .physReg = ARG_REGS[p] }, (MachOperand){ .kind = MO_NONE });
        } else {
            fprintf(stderr,
                    "instr_selector: parametro #%d di '%s' passato su stack "
                    "(>%d parametri) non ancora supportato\n",
                    p + 1, irf->name, NUM_ARG_REGS);
        }
    }
}

/* =========================================================================
 * Per-opcode instruction selection handlers
 * =========================================================================
 * One function per IR opcode (or opcode group sharing identical lowering),
 * so the main select_function() loop reduces to a single dispatch switch.
 * ========================================================================= */

static void select_label(MachFunction *f, const IRInstr *in) {
    mfunc_emit(f, MACH_LABEL, (MachOperand){ .kind = MO_LABEL, .labelId = in->dst.data.labelId }, (MachOperand){ .kind = MO_NONE }, (MachOperand){ .kind = MO_NONE });
}

static void select_goto(MachFunction *f, const IRInstr *in) {
    mfunc_emit(f, MACH_JMP, (MachOperand){ .kind = MO_LABEL, .labelId = in->dst.data.labelId }, (MachOperand){ .kind = MO_NONE }, (MachOperand){ .kind = MO_NONE });
}

/**
 * @brief Lower IR_IF_FALSE, fusing with a pending comparison into a single
 *        CMP + Jcc when possible (see PendingCmp doc), otherwise TEST+JE
 *        on an ordinary 0/1-valued vreg.
 */
static void select_if_false(VarMap *operandToVreg, MachFunction *f, PendingCmp *pcmp,
                             const IRInstr *in) {
    int cond_vreg = varmap_operand_id(operandToVreg, in->src1);
    int lbl       = in->dst.data.labelId;

    if (pcmp->active && cond_vreg == pcmp->dstVreg) {
        /* CMP + Jcc fusion: skip SETcc/MOVSX entirely. */
        const IRInstr *ci = pcmp->instr;
        MachOperand lhs   = operand_to_mach(&ci->src1, operandToVreg, f);
        MachOperand rhs   = operand_to_mach(&ci->src2, operandToVreg, f);
        IROp cmpOp        = ci->op;

        if (lhs.kind == MO_IMM && rhs.kind != MO_IMM) {
            MachOperand t = lhs; lhs = rhs; rhs = t;
            cmpOp = flip_cmp(cmpOp);
        }
        if (lhs.kind == MO_IMM) {
            int tmp = mfunc_new_vreg(f);
            mfunc_emit(f, MACH_MOV, (MachOperand){ .kind = MO_VREG, .vregId = tmp }, lhs, (MachOperand){ .kind = MO_NONE });
            lhs = (MachOperand){ .kind = MO_VREG, .vregId = tmp };
        }
        mfunc_emit(f, MACH_CMP, lhs, rhs, (MachOperand){ .kind = MO_NONE });

        // IR_IF_FALSE jumps when the condition is FALSE, so the emitted
        // Jcc must test the NEGATED comparison
        MachOpCode jcc;
        switch (cmpOp) {
        case IR_LT: jcc = MACH_JGE; break; case IR_LE: jcc = MACH_JG;  break;
        case IR_GT: jcc = MACH_JLE; break; case IR_GE: jcc = MACH_JL;  break;
        case IR_EQ: jcc = MACH_JNE; break; case IR_NE: jcc = MACH_JE;  break;
        default:    jcc = MACH_JMP; break;
        }
        mfunc_emit(f, jcc, (MachOperand){ .kind = MO_LABEL, .labelId = lbl }, (MachOperand){ .kind = MO_NONE }, (MachOperand){ .kind = MO_NONE });
        pcmp->active = 0;
    } else {
        // no fusable comparison pending: condition is an ordinary
        // 0/1-valued vreg, test it directly with TEST+JE
        mfunc_emit(f, MACH_TEST, (MachOperand){ .kind = MO_VREG, .vregId = cond_vreg }, (MachOperand){ .kind = MO_VREG, .vregId = cond_vreg }, (MachOperand){ .kind = MO_NONE });
        mfunc_emit(f, MACH_JE,   (MachOperand){ .kind = MO_LABEL, .labelId = lbl }, (MachOperand){ .kind = MO_NONE }, (MachOperand){ .kind = MO_NONE });
    }
}

static void select_assign(VarMap *operandToVreg, MachFunction *f, const IRInstr *in) {
    int         dst = varmap_operand_id(operandToVreg, in->dst);
    MachOperand src = operand_to_mach(&in->src1, operandToVreg, f);
    mfunc_emit(f, MACH_MOV, (MachOperand){ .kind = MO_VREG, .vregId = dst }, src, (MachOperand){ .kind = MO_NONE });
}

/** @brief Lowering already done in IR: only emits the LEA here. */
static void select_global_addr(VarMap *operandToVreg, MachFunction *f, const IRInstr *in,
                                const IRGlobalVar *globals, int globalCount) {
    int dst = varmap_operand_id(operandToVreg, in->dst);
    int idx = find_global_idx(in->src1.data.globalOffset, globals, globalCount);
    /* idx >= 0 guaranteed: lowering only emits IR_GLOBAL_ADDR for valid globals. */
    mfunc_emit(f, MACH_LEA, (MachOperand){ .kind = MO_VREG, .vregId = dst }, (MachOperand){ .kind = MO_GLOBAL, .globalName = globals[idx].name }, (MachOperand){ .kind = MO_NONE });
}

/** @brief IR_ADD/IR_SUB, reusing dst as an operand in place where possible. */
static void select_add_sub(VarMap *operandToVreg, MachFunction *f, const IRInstr *in) {
    int         dst = varmap_operand_id(operandToVreg, in->dst);
    MachOperand lhs = operand_to_mach(&in->src1, operandToVreg, f);
    MachOperand rhs = operand_to_mach(&in->src2, operandToVreg, f);
    MachOpCode  mop = (in->op == IR_ADD) ? MACH_ADD : MACH_SUB;
    int lhs_id = (lhs.kind == MO_VREG) ? lhs.vregId : -1;
    int rhs_id = (rhs.kind == MO_VREG) ? rhs.vregId : -1;

    // x86 ADD/SUB are two-operand (dst is also an implicit source): try to
    // reuse dst as one of the operands in place to avoid an extra MOV
    if (lhs_id == dst) {
        mfunc_emit(f, mop, (MachOperand){ .kind = MO_VREG, .vregId = dst }, rhs, (MachOperand){ .kind = MO_NONE });
    } else if (in->op == IR_ADD && rhs_id == dst) {
        // addition is commutative: dst already holds rhs
        mfunc_emit(f, MACH_ADD, (MachOperand){ .kind = MO_VREG, .vregId = dst }, lhs, (MachOperand){ .kind = MO_NONE });
    } else if (in->op == IR_SUB && rhs_id == dst) {
        // subtraction is NOT commutative: -(dst) + lhs == lhs - dst
        mfunc_emit(f, MACH_NEG, (MachOperand){ .kind = MO_VREG, .vregId = dst }, (MachOperand){ .kind = MO_NONE }, (MachOperand){ .kind = MO_NONE });
        mfunc_emit(f, MACH_ADD, (MachOperand){ .kind = MO_VREG, .vregId = dst }, lhs, (MachOperand){ .kind = MO_NONE });
    } else {
        mfunc_emit(f, MACH_MOV, (MachOperand){ .kind = MO_VREG, .vregId = dst }, lhs, (MachOperand){ .kind = MO_NONE });
        mfunc_emit(f, mop,      (MachOperand){ .kind = MO_VREG, .vregId = dst }, rhs, (MachOperand){ .kind = MO_NONE });
    }
}

/** @brief IR_MUL, with power-of-2 immediates strength-reduced to a shift. */
static void select_mul(VarMap *operandToVreg, MachFunction *f, const IRInstr *in) {
    int         dst  = varmap_operand_id(operandToVreg, in->dst);
    MachOperand src1 = operand_to_mach(&in->src1, operandToVreg, f);
    MachOperand src2 = operand_to_mach(&in->src2, operandToVreg, f);

    MachOperand reg_side = src1, imm_side = src2;
    if (src1.kind == MO_IMM && src2.kind != MO_IMM) {
        reg_side = src2; imm_side = src1;
    }

    if (imm_side.kind == MO_IMM && imm_side.imm > 0 &&
        (imm_side.imm & (imm_side.imm - 1)) == 0) {
        int shift = 0;
         long v = imm_side.imm;
        while (v > 1) { shift++; v >>= 1; }
        int reg_id = (reg_side.kind == MO_VREG) ? reg_side.vregId : -1;
        if (reg_id != dst)  mfunc_emit(f, MACH_MOV, (MachOperand){ .kind = MO_VREG, .vregId = dst }, reg_side, (MachOperand){ .kind = MO_NONE });
        mfunc_emit(f, MACH_SAL, (MachOperand){ .kind = MO_VREG, .vregId = dst }, (MachOperand){ .kind = MO_IMM, .imm = shift }, (MachOperand){ .kind = MO_NONE });
    } else {
        // general IMUL path: commutative, so either operand aliasing dst
        // can be reused directly
        int src1_id = (src1.kind == MO_VREG) ? src1.vregId : -1;
        int src2_id = (src2.kind == MO_VREG) ? src2.vregId : -1;
        if (src1_id == dst) {
            mfunc_emit(f, MACH_IMUL, (MachOperand){ .kind = MO_VREG, .vregId = dst }, src2, (MachOperand){ .kind = MO_NONE });
        } else if (src2_id == dst) {
            mfunc_emit(f, MACH_IMUL, (MachOperand){ .kind = MO_VREG, .vregId = dst }, src1, (MachOperand){ .kind = MO_NONE });
        } else {
            mfunc_emit(f, MACH_MOV,  (MachOperand){ .kind = MO_VREG, .vregId = dst }, src1, (MachOperand){ .kind = MO_NONE });
            mfunc_emit(f, MACH_IMUL, (MachOperand){ .kind = MO_VREG, .vregId = dst }, src2, (MachOperand){ .kind = MO_NONE });
        }
    }
}

/** @brief IR_DIV/IR_MOD via RDX:RAX IDIV; both operands forced into registers. */
static void select_div_mod(VarMap *operandToVreg, MachFunction *f, const IRInstr *in) {
    int dst = varmap_operand_id(operandToVreg, in->dst);
    int lhs = load_operand(&in->src1, operandToVreg, f);
    int rhs = load_operand(&in->src2, operandToVreg, f);
    mfunc_emit(f, MACH_MOV,  (MachOperand){ .kind = MO_PHYS, .physReg = PHYS_RAX }, (MachOperand){ .kind = MO_VREG, .vregId = lhs }, (MachOperand){ .kind = MO_NONE });
    mfunc_emit(f, MACH_CQO,  (MachOperand){ .kind = MO_NONE }, (MachOperand){ .kind = MO_NONE }, (MachOperand){ .kind = MO_NONE }); // sign-extend RAX into RDX:RAX
    mfunc_emit(f, MACH_IDIV, (MachOperand){ .kind = MO_VREG, .vregId = rhs }, (MachOperand){ .kind = MO_NONE }, (MachOperand){ .kind = MO_NONE });
    // IDIV leaves quotient in RAX, remainder in RDX
    MachPhysReg res = (in->op == IR_DIV) ? PHYS_RAX : PHYS_RDX;
    mfunc_emit(f, MACH_MOV, (MachOperand){ .kind = MO_VREG, .vregId = dst }, (MachOperand){ .kind = MO_PHYS, .physReg = res }, (MachOperand){ .kind = MO_NONE });
}

static void select_neg(VarMap *operandToVreg, MachFunction *f, const IRInstr *in) {
    int         dst = varmap_operand_id(operandToVreg, in->dst);
    MachOperand src = operand_to_mach(&in->src1, operandToVreg, f);
    // MACH_NEG is in-place; only copy src into dst first if not already there
    if ((src.kind == MO_VREG ? src.vregId : -1) != dst)
        mfunc_emit(f, MACH_MOV, (MachOperand){ .kind = MO_VREG, .vregId = dst }, src, (MachOperand){ .kind = MO_NONE });
    mfunc_emit(f, MACH_NEG, (MachOperand){ .kind = MO_VREG, .vregId = dst }, (MachOperand){ .kind = MO_NONE }, (MachOperand){ .kind = MO_NONE });
}

static void select_not(VarMap *operandToVreg, MachFunction *f, const IRInstr *in) {
    // logical NOT: TEST sv,sv sets ZF iff sv is zero; SETE captures it;
    // MOVSX widens the byte result
    int dst = varmap_operand_id(operandToVreg, in->dst);
    int sv  = load_operand(&in->src1, operandToVreg, f);
    mfunc_emit(f, MACH_TEST,  (MachOperand){ .kind = MO_VREG, .vregId = sv },  (MachOperand){ .kind = MO_VREG, .vregId = sv },  (MachOperand){ .kind = MO_NONE });
    mfunc_emit(f, MACH_SETE,  (MachOperand){ .kind = MO_PHYS, .physReg = PHYS_AL }, (MachOperand){ .kind = MO_NONE }, (MachOperand){ .kind = MO_NONE });
    mfunc_emit(f, MACH_MOVSX, (MachOperand){ .kind = MO_VREG, .vregId = dst }, (MachOperand){ .kind = MO_PHYS, .physReg = PHYS_AL }, (MachOperand){ .kind = MO_NONE });
}

/** @brief Defer comparisons (IR_LT..IR_NE) for possible CMP+Jcc fusion. */
static void select_defer_comparison(VarMap *operandToVreg, PendingCmp *pcmp, const IRInstr *in) {
    int dst = varmap_operand_id(operandToVreg, in->dst);
    pcmp->active  = 1;
    pcmp->instr   = in;
    pcmp->dstVreg = dst;
}

static void select_load_arr(VarMap *operandToVreg, MachFunction *f, const IRInstr *in) {
    int dst  = varmap_operand_id(operandToVreg, in->dst);
    int base = varmap_operand_id(operandToVreg, in->src1);
    if (in->src2.kind == OPND_CONST_INT) {
        // constant index folded into the addressing-mode displacement
        int disp = in->src2.data.intVal * 8;
        mfunc_emit(f, MACH_LOAD, (MachOperand){ .kind = MO_VREG, .vregId = dst }, (MachOperand){ .kind = MO_MEM, .mem = { .baseVreg = base, .indexVreg = -1, .scale = 0, .disp = disp } }, (MachOperand){ .kind = MO_NONE });
    } else {
        // dynamic index: real SIB addressing mode (base + idx*8)
        int idx = load_operand(&in->src2, operandToVreg, f);
        mfunc_emit(f, MACH_LOAD, (MachOperand){ .kind = MO_VREG, .vregId = dst }, (MachOperand){ .kind = MO_MEM, .mem = { .baseVreg = base, .indexVreg = idx, .scale = 8, .disp = 0 } }, (MachOperand){ .kind = MO_NONE });
    }
}

static void select_store_arr(VarMap *operandToVreg, MachFunction *f, const IRInstr *in) {
    int base = varmap_operand_id(operandToVreg, in->dst);
    if (in->src1.kind == OPND_CONST_INT) {
        int disp = in->src1.data.intVal * 8;
        int src  = load_operand(&in->src2, operandToVreg, f);
        mfunc_emit(f, MACH_STORE, (MachOperand){ .kind = MO_MEM, .mem = { .baseVreg = base, .indexVreg = -1, .scale = 0, .disp = disp } }, (MachOperand){ .kind = MO_VREG, .vregId = src }, (MachOperand){ .kind = MO_NONE });
    } else {
        int idx = load_operand(&in->src1, operandToVreg, f);
        int src = load_operand(&in->src2, operandToVreg, f);
        mfunc_emit(f, MACH_STORE, (MachOperand){ .kind = MO_MEM, .mem = { .baseVreg = base, .indexVreg = idx, .scale = 8, .disp = 0 } }, (MachOperand){ .kind = MO_VREG, .vregId = src }, (MachOperand){ .kind = MO_NONE });
    }
}

/** @brief Stage one call argument; actual placement happens at IR_CALL. */
static void select_param(VarMap *operandToVreg, MachFunction *f, const IRInstr *in,
                          PendingArgs *args) {
    MachOperand sv = operand_to_mach(&in->src1, operandToVreg, f);
    int sv_vreg;
    if (sv.kind == MO_IMM) {
        sv_vreg = mfunc_new_vreg(f);
        mfunc_emit(f, MACH_MOV, (MachOperand){ .kind = MO_VREG, .vregId = sv_vreg }, sv, (MachOperand){ .kind = MO_NONE });
    } else {
        sv_vreg = sv.vregId;
    }
    if (args->count < MAX_PARAMS)
        args->vregs[args->count++] = sv_vreg;
}

/** @brief Emit the full call sequence (stack args, register args, CALL, cleanup). */
static void select_call(VarMap *operandToVreg, MachFunction *f, const IRInstr *in,
                         PendingArgs *args) {
    int n = args->count;
    // overflow args (beyond the 6 ABI registers) pushed in REVERSE order
    // (rightmost first) so they end up left-to-right in memory
    for (int k = n - 1; k >= NUM_ARG_REGS; k--)
        mfunc_emit(f, MACH_PUSH, (MachOperand){ .kind = MO_VREG, .vregId = args->vregs[k] }, (MachOperand){ .kind = MO_NONE }, (MachOperand){ .kind = MO_NONE });
    int reg_args = (n < NUM_ARG_REGS) ? n : NUM_ARG_REGS;
    for (int k = 0; k < reg_args; k++)
        mfunc_emit(f, MACH_MOV, (MachOperand){ .kind = MO_PHYS, .physReg = ARG_REGS[k] }, (MachOperand){ .kind = MO_VREG, .vregId = args->vregs[k] }, (MachOperand){ .kind = MO_NONE });
    mfunc_emit(f, MACH_CALL, (MachOperand){ .kind = MO_FUNC, .func = in->src1.data.funcName }, (MachOperand){ .kind = MO_NONE }, (MachOperand){ .kind = MO_NONE });
    // caller cleans up any stack-pushed arguments after the call returns
    int extra = n - NUM_ARG_REGS;
    if (extra > 0) {
        int adj = mfunc_new_vreg(f);
        mfunc_emit(f, MACH_MOV, (MachOperand){ .kind = MO_VREG, .vregId = adj }, (MachOperand){ .kind = MO_IMM, .imm = extra * 8L }, (MachOperand){ .kind = MO_NONE });
        mfunc_emit(f, MACH_ADD, (MachOperand){ .kind = MO_PHYS, .physReg = PHYS_RSP }, (MachOperand){ .kind = MO_VREG, .vregId = adj }, (MachOperand){ .kind = MO_NONE });
    }
    // return value convention: RAX -> destination vreg
    int dst = varmap_operand_id(operandToVreg, in->dst);
    mfunc_emit(f, MACH_MOV, (MachOperand){ .kind = MO_VREG, .vregId = dst }, (MachOperand){ .kind = MO_PHYS, .physReg = PHYS_RAX }, (MachOperand){ .kind = MO_NONE });
    args->count = 0; // reset staging buffer for the next call site
}

static void select_return(VarMap *operandToVreg, MachFunction *f, const IRInstr *in) {
    int sv = load_operand(&in->src1, operandToVreg, f);
    mfunc_emit(f, MACH_MOV, (MachOperand){ .kind = MO_PHYS, .physReg = PHYS_RAX }, (MachOperand){ .kind = MO_VREG, .vregId = sv }, (MachOperand){ .kind = MO_NONE });
    mfunc_emit(f, MACH_RET, (MachOperand){ .kind = MO_NONE }, (MachOperand){ .kind = MO_NONE }, (MachOperand){ .kind = MO_NONE });
}

/* =========================================================================
 * select_function — per-function instruction selection (orchestrator)
 * ========================================================================= */

static MachFunction *select_function(const IRFunction *irf,
                                      const IRGlobalVar *globals,
                                      int globalCount) {
    MachFunction *f = mfunc_create(irf->name);
    g_curLoopDepth = 0; /* reset: must not leak from the previously
                            selected function's last instruction */

    // VarMap holding the operand -> vreg id mapping for this function
    // (1:1: VarMap ids double as vreg ids in this backend)
    VarMap operandToVreg = varmap_init();
    select_prescan_and_sync_vregs(irf, &operandToVreg, f);

    mfunc_emit(f, MACH_FUNC_BEGIN, (MachOperand){ .kind = MO_NONE }, (MachOperand){ .kind = MO_NONE }, (MachOperand){ .kind = MO_NONE });
    select_bind_params(irf, &operandToVreg, f);

    PendingArgs args = { .count = 0 };
    PendingCmp  pcmp = { .active = 0 };

    for (int i = 0; i < irf->count; i++) {
        const IRInstr *in = &irf->instrs[i];
        g_curLoopDepth = in->loopDepth;

        // flush a deferred comparison if the current instruction can't
        // fuse with it (fusion only applies when the very next instruction
        // is the IF_FALSE consuming exactly this comparison's result)
        if (pcmp.active) {
            int must_materialize = 1;
            if (in->op == IR_IF_FALSE)
                must_materialize = (varmap_operand_id(&operandToVreg, in->src1) != pcmp.dstVreg);
            if (must_materialize)
                flush_pending_cmp(&pcmp, &operandToVreg, f);
        }

        switch (in->op) {
        case IR_LABEL:       select_label(f, in); break;
        case IR_GOTO:        select_goto(f, in); break;
        case IR_IF_FALSE:    select_if_false(&operandToVreg, f, &pcmp, in); break;
        case IR_ASSIGN:      select_assign(&operandToVreg, f, in); break;
        case IR_GLOBAL_ADDR: select_global_addr(&operandToVreg, f, in, globals, globalCount); break;
        case IR_ADD: case IR_SUB: select_add_sub(&operandToVreg, f, in); break;
        case IR_MUL:          select_mul(&operandToVreg, f, in); break;
        case IR_DIV: case IR_MOD: select_div_mod(&operandToVreg, f, in); break;
        case IR_NEG:          select_neg(&operandToVreg, f, in); break;
        case IR_NOT:           select_not(&operandToVreg, f, in); break;
        case IR_LT: case IR_LE: case IR_GT: case IR_GE:
        case IR_EQ: case IR_NE: select_defer_comparison(&operandToVreg, &pcmp, in); break;
        case IR_LOAD_ARR:     select_load_arr(&operandToVreg, f, in); break;
        case IR_STORE_ARR:    select_store_arr(&operandToVreg, f, in); break;
        case IR_PARAM:        select_param(&operandToVreg, f, in, &args); break;
        case IR_CALL:         select_call(&operandToVreg, f, in, &args); break;
        case IR_RETURN:       select_return(&operandToVreg, f, in); break;
        }
    }

    // a comparison could be the very last instruction of the function body
    // (e.g. "return a < b;" without an intervening branch): flush it here
    flush_pending_cmp(&pcmp, &operandToVreg, f);

    // frame size: 8 bytes per vreg slot, rounded up to 16-byte alignment
    int raw      = f->nextVreg * 8;
    f->frameSize = (raw + 15) & ~15;

    varmap_destroy(&operandToVreg);
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
    case MO_GLOBAL: fprintf(out, "%s(%%rip)",   o->globalName);           break; // RIP-relative addressing
    case MO_STACK:  fprintf(out, "-%d(%%rbp)",  o->stackOff);             break; // spill slots: always negative offset from rbp
    case MO_MEM:
        // AT&T syntax: disp(base,index,scale)
        if (o->mem.disp) fprintf(out, "%d", o->mem.disp);
        fprintf(out, "(");
        if (o->mem.baseVreg  >= 0) fprintf(out, "%s", phys_name64[o->mem.baseVreg]);
        if (o->mem.indexVreg >= 0) fprintf(out, ",%s,%d",
                                            phys_name64[o->mem.indexVreg],
                                            o->mem.scale);
        fprintf(out, ")");
        break;
    case MO_FIMM:
        // hex encoding of the raw bit pattern (no float immediates emitted
        // by this backend today; kept for completeness/future SSE support)
        fprintf(out, "$0x%x", (unsigned)(int)o->fimm);
        break;
    }
}
/* =========================================================================
 * Global variable section emission (.bss / .data)
 * ========================================================================= */

/** @brief Emit .bss entries for every zero-initialised global (initCount == 0). */
static void emit_bss_section(FILE *out, const IRProgram *ir) {
    int has_bss = 0;
    for (int i = 0; i < ir->globalCount; i++)
        if (ir->globals[i].initCount == 0) { has_bss = 1; break; }
    if (!has_bss) return;

    fprintf(out, "\t.bss\n");
    for (int i = 0; i < ir->globalCount; i++) {
        const IRGlobalVar *g = &ir->globals[i];
        if (g->initCount > 0) continue;
        int nelems = g->isArray ? g->arraySize : 1;
        fprintf(out, "\t.globl %s\n%s:\n", g->name, g->name);
        fprintf(out, "\t.zero %d\n", nelems * 8); // 8 bytes/element (int and float both stored as 8-byte slots)
    }
}

/** @brief Emit .data entries for every explicitly-initialised global. */
static void emit_data_section(FILE *out, const IRProgram *ir) {
    int has_data = 0;
    for (int i = 0; i < ir->globalCount; i++)
        if (ir->globals[i].initCount > 0) { has_data = 1; break; }
    if (!has_data) return;

    fprintf(out, "\t.data\n");
    for (int i = 0; i < ir->globalCount; i++) {
        const IRGlobalVar *g = &ir->globals[i];
        if (g->initCount == 0) continue;
        int nelems = g->isArray ? g->arraySize : 1;
        fprintf(out, "\t.globl %s\n%s:\n", g->name, g->name);
        for (int j = 0; j < nelems; j++) {
            // elements past the explicit initializer list default to 0
            // (C semantics: partial array initializers zero-fill the rest)
            long val = (j < g->initCount) ? g->initVals[j] : 0L;
            fprintf(out, "\t.quad %ld\n", val);
        }
    }
}

static void emit_globals(FILE *out, const IRProgram *ir) {
    if (!ir || ir->globalCount == 0) return;
    emit_bss_section(out, ir);
    emit_data_section(out, ir);
}

/* =========================================================================
 * Per-instruction printing
 * ========================================================================= */

/**
 * @brief Try to print @p in via one of the fixed/irregular AT&T encodings
 *        that don't fit the generic "mnemonic src, dst" pattern (labels,
 *        prologue/epilogue, single-operand forms, jumps, SETcc — which
 *        prints no operand at all — ...).
 *
 * @param frameSize  Enclosing function's frame size, needed only for the
 *                    MACH_FUNC_BEGIN prologue's `subq`.
 * @return 1 if @p in was fully printed (caller moves to the next
 *         instruction), 0 if it must go through emit_generic_instr().
 */
static int emit_fixed_encoding_instr(const MachInstr *in, int frameSize, FILE *out) {
    switch (in->op) {
    case MACH_LABEL:
        fprintf(out, ".L%d:\n", in->dst.labelId); return 1;
    case MACH_FUNC_BEGIN:
        // standard x86-64 prologue: save caller's frame pointer,
        // establish new frame, reserve local storage
        fprintf(out, "\tpushq\t%%rbp\n");
        fprintf(out, "\tmovq\t%%rsp, %%rbp\n");
        if (frameSize > 0)
            fprintf(out, "\tsubq\t$%d, %%rsp\n", frameSize);
        return 1;
    case MACH_RET:
        // `leave` restores rsp/rbp in one instruction
        fprintf(out, "\tleave\n\tret\n"); return 1;
    case MACH_CQO:
        fprintf(out, "\tcqo\n"); return 1;
    case MACH_IDIV:
        fprintf(out, "\tidivq\t"); emit_operand(&in->dst, out);
        fprintf(out, "\n"); return 1;
    case MACH_NEG:
        fprintf(out, "\tnegq\t"); emit_operand(&in->dst, out);
        fprintf(out, "\n"); return 1;
    case MACH_NOT:
        fprintf(out, "\tnotq\t"); emit_operand(&in->dst, out);
        fprintf(out, "\n"); return 1;
    case MACH_PUSH:
        fprintf(out, "\tpushq\t"); emit_operand(&in->dst, out);
        fprintf(out, "\n"); return 1;
    case MACH_POP:
        fprintf(out, "\tpopq\t"); emit_operand(&in->dst, out);
        fprintf(out, "\n"); return 1;
    case MACH_CALL:
        fprintf(out, "\tcall\t"); emit_operand(&in->dst, out);
        fprintf(out, "\n"); return 1;
    case MACH_LEA:
        fprintf(out, "\tleaq\t");
        emit_operand(&in->src1, out);
        fprintf(out, ", ");
        emit_operand(&in->dst, out);
        fprintf(out, "\n"); return 1;
    // SETcc variants: fixed destination (%al), no operand printing needed
    case MACH_SETE:  fprintf(out, "\tsete\t%%al\n");  return 1;
    case MACH_SETNE: fprintf(out, "\tsetne\t%%al\n"); return 1;
    case MACH_SETL:  fprintf(out, "\tsetl\t%%al\n");  return 1;
    case MACH_SETLE: fprintf(out, "\tsetle\t%%al\n"); return 1;
    case MACH_SETG:  fprintf(out, "\tsetg\t%%al\n");  return 1;
    case MACH_SETGE: fprintf(out, "\tsetge\t%%al\n"); return 1;
    // unconditional/conditional jumps: label operand always in dst
    case MACH_JMP:   fprintf(out, "\tjmp\t.L%d\n",  in->dst.labelId); return 1;
    case MACH_JE:    fprintf(out, "\tje\t.L%d\n",   in->dst.labelId); return 1;
    case MACH_JNE:   fprintf(out, "\tjne\t.L%d\n",  in->dst.labelId); return 1;
    case MACH_JL:    fprintf(out, "\tjl\t.L%d\n",   in->dst.labelId); return 1;
    case MACH_JLE:   fprintf(out, "\tjle\t.L%d\n",  in->dst.labelId); return 1;
    case MACH_JG:    fprintf(out, "\tjg\t.L%d\n",   in->dst.labelId); return 1;
    case MACH_JGE:   fprintf(out, "\tjge\t.L%d\n",  in->dst.labelId); return 1;
    default: return 0; // falls through to the generic two-operand printer
    }
}

/**
 * @brief Print @p in via the generic "mnemonic src, dst" AT&T pattern.
 *
 * Only reached for opcodes emit_fixed_encoding_instr() didn't claim
 * (MOV/MOVSX/ADD/SUB/IMUL/SAL/XOR/CMP/TEST/LOAD/STORE, or "???" defensively).
 */
static void emit_generic_instr(const MachInstr *in, FILE *out) {
    const char *mnem = NULL;
    switch (in->op) {
    case MACH_MOV:   mnem = "movq";   break;
    case MACH_MOVSX: mnem = "movsbq"; break; // sign-extend byte -> quad (SETcc result -> full reg)
    case MACH_ADD:   mnem = "addq";   break;
    case MACH_SUB:   mnem = "subq";   break;
    case MACH_IMUL:  mnem = "imulq";  break;
    case MACH_SAL:   mnem = "salq";   break;
    case MACH_XOR:   mnem = "xorq";   break;
    case MACH_CMP:   mnem = "cmpq";   break;
    case MACH_TEST:  mnem = "testq";  break;
    case MACH_LOAD:  mnem = "movq";   break; // LOAD/STORE both lower to plain movq; direction differs
    case MACH_STORE: mnem = "movq";   break;
    default:         mnem = "???";    break; // should not happen for a well-formed MachInstr stream
    }

    fprintf(out, "\t%s\t", mnem);

    // AT&T operand order is "src, dst". LOAD/STORE address the memory
    // operand via src1/dst depending on direction; other binary ops use
    // src1 as the explicit source (dst is implicitly both input/output,
    // as arranged by the in-place-reuse logic in select_function).
    if (in->op == MACH_LOAD || in->op == MACH_STORE) {
        emit_operand(&in->src1, out);
        fprintf(out, ", ");
        emit_operand(&in->dst, out);
    } else if (in->src2.kind != MO_NONE ) {
        // shouldn't normally happen post-selection, kept defensively
        emit_operand(&in->src1, out);
        fprintf(out, ", ");
        emit_operand(&in->dst, out);
    } else if (in->src1.kind != MO_NONE ) {
        emit_operand(&in->src1, out);
        if (in->dst.kind != MO_NONE) {
            fprintf(out, ", ");
            emit_operand(&in->dst, out);
        }
    } else {
        // single-operand instruction with only dst set
        emit_operand(&in->dst, out);
    }
    fprintf(out, "\n");
}

/** @brief Print every instruction of one function body, in order. */
static void emit_function_body(const MachFunction *f, FILE *out) {
    for (int i = 0; i < f->count; i++) {
        const MachInstr *in = &f->instrs[i];
        if (emit_fixed_encoding_instr(in, f->frameSize, out)) continue;
        emit_generic_instr(in, out);
    }
}


void isel_emit_asm(const MachProgram *mp, const IRProgram *ir, FILE *out) {
    emit_globals(out, ir);

    fprintf(out, "\t.text\n");
    for (int fi = 0; fi < mp->count; fi++) {
        const MachFunction *f = mp->functions[fi];
        fprintf(out, "\t.globl %s\n%s:\n", f->name, f->name);
        emit_function_body(f, out);
        fprintf(out, "\n");
    }
}
void mach_free(MachProgram *mp) {
    if (!mp) return;
    for (int i = 0; i < mp->count; i++) {
        free(mp->functions[i]->instrs);
        free(mp->functions[i]);
    }
    free(mp->functions);
    free(mp);
}