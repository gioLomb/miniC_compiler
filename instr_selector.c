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
#include "parser/errorCollector.h"

// indexed by MachPhysReg enum value; last entry ("%al") is the 8-bit
// alias of PHYS_RAX, used only when printing SETcc destinations
static const char *phys_name64[] = {
    "%rax", "%rcx", "%rdx", "%rsi", "%rdi",
    "%r8",  "%r9",  "%r10", "%r11",
    "%rbx", "%r12", "%r13", "%r14", "%r15",
    "%rbp", "%rsp", "%al"
};

static const char *phys_name_xmm[PHYS_XMM_COUNT] = {
    "%xmm0","%xmm1","%xmm2","%xmm3","%xmm4","%xmm5","%xmm6","%xmm7"
};
static inline int is_xmm_phys(int p) { return p >= PHYS_XMM0 && p < PHYS_XMM0 + PHYS_XMM_COUNT; }

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
typedef struct { Operand ops[MAX_PARAMS]; int count; } PendingArgs;

/**
 * @brief State for a comparison instruction whose CMP/SETcc emission is
 *        deferred until we know how it will be consumed.
 *
 * IR comparisons (IR_LT, IR_EQ, ...) normally materialise a 0/1 value via
 * CMP + SETcc + MOVSX. But if the very next IR instruction is an
 * IR_IF_FALSE testing that same result, the value never needs to exist as
 * a byte in memory/register at all — a single CMP + Jcc suffices. Deferring
 * the emission lets isel_select_function() choose the cheaper path once it sees
 * how the comparison result is used.
 */
typedef struct {
    int            active;   /**< 1 if a comparison is currently pending materialisation. */
    const IRInstr *instr;    /**< The IR_LT/IR_EQ/... instruction being deferred. */
    int            dstVreg;  /**< vreg that would hold the materialised 0/1 result. */
} PendingCmp;


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
    if (__builtin_expect(f->count == f->capacity,0)) {
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
 * Unlike isel_operand_to_mach(), this ALWAYS returns a vreg id (never an
 * immediate MachOperand). Used where the target machine instruction has
 * no immediate-operand form (e.g. IDIV divisor, PUSH source) and the
 * value must sit in a register regardless of how it was expressed in IR.
 *
 * @param op            IR operand to materialise.
 * @param operandToVreg VarMap providing the operand -> vreg id mapping.
 * @param f             Machine function new MOVs (for constants) are emitted into.
 */
static int isel_load_operand(const Operand *op, VarMap *operandToVreg, MachFunction *f) {
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
 * isel_load_operand() would otherwise emit.
 *
 * @param op            IR operand to convert.
 * @param operandToVreg VarMap providing the operand -> vreg id mapping.
 * @param mf            Unused (kept for call-site symmetry with isel_load_operand()).
 */
static inline MachOperand isel_operand_to_mach(const Operand *op, VarMap *operandToVreg) {
    switch (op->kind) {
    case OPND_CONST_INT:   return (MachOperand){ .kind = MO_IMM, .imm = op->data.intVal };
    case OPND_CONST_FLOAT: {
        // same bit-reinterpretation trick as isel_load_operand()
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
static int isel_find_global_idx(int symOff,
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
static inline IROp isel_flip_cmp(IROp op) {
    switch (op) {
    case IR_LT: return IR_GT; case IR_GT: return IR_LT;
    case IR_LE: return IR_GE; case IR_GE: return IR_LE;
    default:    return op; // EQ/NE are symmetric: no flip needed
    }
}

// maps an IR comparison opcode to the x86 SETcc variant that materialises
// its boolean result into %al
static inline MachOpCode isel_comparison_to_setcc(IROp cmpOp) {
    switch (cmpOp) {
    case IR_LT: return MACH_SETL;  case IR_LE: return MACH_SETLE;
    case IR_GT: return MACH_SETG;  case IR_GE: return MACH_SETGE;
    case IR_EQ: return MACH_SETE;  case IR_NE: return MACH_SETNE;
    default:    return MACH_SETE;
    }
}


/* =========================================================================
 * Float slot manager
 * ========================================================================= */

/**
 * @brief Permanent stack-slot bookkeeping for float-typed operands.
 *
 * Float values never occupy a MO_VREG and never enter the Chaitin-Briggs
 * graph coloring pipeline: every float-typed var/temp gets one dedicated
 * 8-byte stack slot for its entire lifetime, and every float instruction
 * round-trips its operands through this memory via a fixed pair of scratch
 * XMM registers (xmm0/xmm1). This keeps interference.c/ra_color.c/
 * ra_spill.c/bucket.c completely untouched (correctness-first tradeoff:
 * no register-level optimisation for float-heavy hot loops — flagged as a
 * possible future improvement, see design notes).
 */
typedef struct {
    int *slotOfId; /**< slotOfId[varmapId] = dense slot index, -1 = unassigned. */
    int  count;    /**< Number of slots handed out so far.                      */
} FloatSlots;

static FloatSlots fslots_create(int cap) {
    FloatSlots fs;
    fs.slotOfId = malloc((size_t)(cap > 0 ? cap : 1) * sizeof(int));
    memset(fs.slotOfId, -1, (size_t)(cap > 0 ? cap : 1) * sizeof(int));
    fs.count = 0;
    return fs;
}
static void fslots_free(FloatSlots *fs) { free(fs->slotOfId); fs->slotOfId = NULL; }

/** Stack offset convention matches isel_emit_operand's "-%d(%%rbp)" printer. */
static MachOperand fslots_operand(FloatSlots *fs, int varmapId) {
    if (fs->slotOfId[varmapId] < 0) fs->slotOfId[varmapId] = fs->count++;
    return (MachOperand){ .kind = MO_STACK, .stackOff = (fs->slotOfId[varmapId] + 1) * 8 };
}

static int isel_load_float_bits(const Operand *op, MachFunction *f) {
    union { float fl; int i; } u; u.fl = op->data.floatVal;
    int tmp = mfunc_new_vreg(f);
    mfunc_emit(f, MACH_MOV, (MachOperand){.kind=MO_VREG,.vregId=tmp},
               (MachOperand){.kind=MO_IMM,.imm=u.i}, (MachOperand){.kind=MO_NONE});
    return tmp;
}

/** Load a float-typed operand (var/temp slot, or a literal constant) into
 *  physical XMM register @p xmmPhys. */
static void isel_load_float_into_xmm(const Operand *op, VarMap *ov, FloatSlots *fs,
                                     MachFunction *f, int xmmPhys) {
    if (op->kind == OPND_CONST_FLOAT) {
        int bits = isel_load_float_bits(op, f);
        mfunc_emit(f, MACH_MOVQ_TO_XMM, (MachOperand){.kind=MO_PHYS,.physReg=xmmPhys},
                   (MachOperand){.kind=MO_VREG,.vregId=bits}, (MachOperand){.kind=MO_NONE});
        return;
    }
    int id = varmap_operand_id(ov, *op);
    mfunc_emit(f, MACH_MOVSS, (MachOperand){.kind=MO_PHYS,.physReg=xmmPhys},
               fslots_operand(fs, id), (MachOperand){.kind=MO_NONE});
}

/** Store physical XMM register @p xmmPhys into float operand @p dst's slot. */
static void isel_store_float_from_xmm(const Operand *dst, VarMap *ov, FloatSlots *fs,
                                      MachFunction *f, int xmmPhys) {
    int id = varmap_operand_id(ov, *dst);
    mfunc_emit(f, MACH_MOVSS, fslots_operand(fs, id),
               (MachOperand){.kind=MO_PHYS,.physReg=xmmPhys}, (MachOperand){.kind=MO_NONE});
}

static void isel_select_float_binop(VarMap *ov, FloatSlots *fs, MachFunction *f,
                                    const IRInstr *in, MachOpCode sseOp) {
    isel_load_float_into_xmm(&in->src1, ov, fs, f, PHYS_XMM0);
    isel_load_float_into_xmm(&in->src2, ov, fs, f, PHYS_XMM1);
    mfunc_emit(f, sseOp, (MachOperand){.kind=MO_PHYS,.physReg=PHYS_XMM0},
               (MachOperand){.kind=MO_PHYS,.physReg=PHYS_XMM1}, (MachOperand){.kind=MO_NONE});
    isel_store_float_from_xmm(&in->dst, ov, fs, f, PHYS_XMM0);
}


/**
 * @brief Force-materialise a pending comparison as CMP + SETcc + MOVSX.
 *
 * Called whenever the next instruction turns out NOT to be a matching
 * IR_IF_FALSE (so fusion is not possible) — the boolean value must be
 * computed the "normal" way after all.
 */
static void isel_flush_pending_cmp(PendingCmp *pcmp, VarMap *operandToVreg, FloatSlots *fs, MachFunction *f) {
    if (!pcmp->active) return;

    const IRInstr *ci = pcmp->instr;

    if (ci->src1.isFloat) {
        isel_load_float_into_xmm(&ci->src1, operandToVreg, fs, f, PHYS_XMM0);
        isel_load_float_into_xmm(&ci->src2, operandToVreg, fs, f, PHYS_XMM1);
        mfunc_emit(f, MACH_UCOMISS, (MachOperand){.kind=MO_PHYS,.physReg=PHYS_XMM0},
                   (MachOperand){.kind=MO_PHYS,.physReg=PHYS_XMM1}, (MachOperand){.kind=MO_NONE});
        mfunc_emit(f, isel_comparison_to_setcc(ci->op), (MachOperand){.kind=MO_PHYS,.physReg=PHYS_AL},
                   (MachOperand){.kind=MO_NONE}, (MachOperand){.kind=MO_NONE});
        mfunc_emit(f, MACH_MOVSX, (MachOperand){.kind=MO_VREG,.vregId=pcmp->dstVreg},
                   (MachOperand){.kind=MO_PHYS,.physReg=PHYS_AL}, (MachOperand){.kind=MO_NONE});
        pcmp->active = 0;
        return;
    }

    MachOperand lhs   = isel_operand_to_mach(&ci->src1, operandToVreg);
    MachOperand rhs   = isel_operand_to_mach(&ci->src2, operandToVreg);
    IROp cmpOp        = ci->op;

    // x86 CMP requires the first operand to be a register; if the IR gave
    // us an immediate on the left, swap operands and flip the comparison
    // sense so the encoded instruction stays valid and semantically equal
    if (lhs.kind == MO_IMM && rhs.kind != MO_IMM) {
        MachOperand t = lhs; lhs = rhs; rhs = t;
        cmpOp = isel_flip_cmp(cmpOp);
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
    mfunc_emit(f, isel_comparison_to_setcc(cmpOp),  (MachOperand){ .kind = MO_PHYS, .physReg = PHYS_AL }, (MachOperand){ .kind = MO_NONE }, (MachOperand){ .kind = MO_NONE });
    mfunc_emit(f, MACH_MOVSX, (MachOperand){ .kind = MO_VREG, .vregId = pcmp->dstVreg }, (MachOperand){ .kind = MO_PHYS, .physReg = PHYS_AL }, (MachOperand){ .kind = MO_NONE });
    pcmp->active = 0;
}

/* =========================================================================
 * Per-function setup helpers
 * ========================================================================= */

/**
 * @brief register every IR operand (including
 *        unused formal parameters) with a stable vreg id, then sync
 *        f->nextVreg so isel-internal temporaries never collide with them.
 */
static void isel_select_prescan(const IRFunction *irf, VarMap *operandToVreg,
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
 * @brief Phase 3-bis of isel_select_function: bind formal parameters to their
 *        ABI registers (System V: rdi,rsi,rdx,rcx,r8,r9).
 *
 * Parameters beyond the 6th (passed on the caller's stack) are not yet
 * supported — flagged with a diagnostic rather than silently miscompiled.
 */
static void isel_select_bind_params(const IRFunction *irf, VarMap *ov, FloatSlots *fs, MachFunction *f) {
    int intCursor = 0, floatCursor = 0;
    for (int p = 0; p < irf->paramCount; p++) {
        Operand *param = &irf->params[p];
        if (param->isFloat) {
            if (floatCursor >= 8) { ec_report("instr_selector: >8 parametri float non supportato\n"); continue; }
            isel_store_float_from_xmm(param, ov, fs, f, PHYS_XMM0 + floatCursor++);
        } else {
            if (intCursor >= NUM_ARG_REGS) { ec_report("instr_selector: parametro oltre %d non supportato\n", NUM_ARG_REGS); continue; }
            int vreg = varmap_operand_id(ov, *param);
            mfunc_emit(f, MACH_MOV, (MachOperand){.kind=MO_VREG,.vregId=vreg},
                       (MachOperand){.kind=MO_PHYS,.physReg=ARG_REGS[intCursor++]}, (MachOperand){.kind=MO_NONE});
        }
    }
}


static void isel_select_label(MachFunction *f, const IRInstr *in) {
    mfunc_emit(f, MACH_LABEL, (MachOperand){ .kind = MO_LABEL, .labelId = in->dst.data.labelId }, (MachOperand){ .kind = MO_NONE }, (MachOperand){ .kind = MO_NONE });
}

static void isel_select_goto(MachFunction *f, const IRInstr *in) {
    mfunc_emit(f, MACH_JMP, (MachOperand){ .kind = MO_LABEL, .labelId = in->dst.data.labelId }, (MachOperand){ .kind = MO_NONE }, (MachOperand){ .kind = MO_NONE });
}

/**
 * @brief Lower IR_IF_FALSE, fusing with a pending comparison into a single
 *        CMP + Jcc when possible (see PendingCmp doc), otherwise TEST+JE
 *        on an ordinary 0/1-valued vreg.
 */
static void isel_select_if_false(VarMap *operandToVreg, FloatSlots *fs, MachFunction *f, PendingCmp *pcmp,
                             const IRInstr *in) {
    int cond_vreg = varmap_operand_id(operandToVreg, in->src1);
    int lbl       = in->dst.data.labelId;

    if (pcmp->active && cond_vreg == pcmp->dstVreg) {
        /* CMP + Jcc fusion: skip SETcc/MOVSX entirely. */
        const IRInstr *ci = pcmp->instr;

        if (ci->src1.isFloat) {
            isel_load_float_into_xmm(&ci->src1, operandToVreg, fs, f, PHYS_XMM0);
            isel_load_float_into_xmm(&ci->src2, operandToVreg, fs, f, PHYS_XMM1);
            mfunc_emit(f, MACH_UCOMISS, (MachOperand){.kind=MO_PHYS,.physReg=PHYS_XMM0},
                       (MachOperand){.kind=MO_PHYS,.physReg=PHYS_XMM1}, (MachOperand){.kind=MO_NONE});
            MachOpCode jcc;
            switch (ci->op) {
            case IR_LT: jcc = MACH_JAE; break; case IR_LE: jcc = MACH_JA;  break;
            case IR_GT: jcc = MACH_JBE; break; case IR_GE: jcc = MACH_JB;  break;
            case IR_EQ: jcc = MACH_JNE; break; case IR_NE: jcc = MACH_JE;  break;
            default:    jcc = MACH_JMP; break;
            }
            mfunc_emit(f, jcc, (MachOperand){ .kind = MO_LABEL, .labelId = lbl }, (MachOperand){ .kind = MO_NONE }, (MachOperand){ .kind = MO_NONE });
            pcmp->active = 0;
            return;
        }

        MachOperand lhs   = isel_operand_to_mach(&ci->src1, operandToVreg);
        MachOperand rhs   = isel_operand_to_mach(&ci->src2, operandToVreg);
        IROp cmpOp        = ci->op;

        if (lhs.kind == MO_IMM && rhs.kind != MO_IMM) {
            MachOperand t = lhs; lhs = rhs; rhs = t;
            cmpOp = isel_flip_cmp(cmpOp);
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

static void isel_select_assign(VarMap *ov, FloatSlots *fs, MachFunction *f, const IRInstr *in) {
    if (in->dst.isFloat) {
        if (in->src1.isFloat || in->src1.kind == OPND_CONST_FLOAT) {
            isel_load_float_into_xmm(&in->src1, ov, fs, f, PHYS_XMM0);
        } else {
            // implicit int -> float widening (see semantic.c is_type_compatible)
            int srcVreg = isel_load_operand(&in->src1, ov, f);
            mfunc_emit(f, MACH_CVTSI2SS, (MachOperand){.kind=MO_PHYS,.physReg=PHYS_XMM0},
                       (MachOperand){.kind=MO_VREG,.vregId=srcVreg}, (MachOperand){.kind=MO_NONE});
        }
        isel_store_float_from_xmm(&in->dst, ov, fs, f, PHYS_XMM0);
        return;
    }
    int         dst = varmap_operand_id(ov, in->dst);
    MachOperand src = isel_operand_to_mach(&in->src1, ov);
    mfunc_emit(f, MACH_MOV, (MachOperand){ .kind = MO_VREG, .vregId = dst }, src, (MachOperand){ .kind = MO_NONE });
}

/** @brief Lowering already done in IR: only emits the LEA here. */
static void isel_select_global_addr(VarMap *operandToVreg, MachFunction *f, const IRInstr *in,
                                const IRGlobalVar *globals, int globalCount) {
    int dst = varmap_operand_id(operandToVreg, in->dst);
    int idx = isel_find_global_idx(in->src1.data.globalOffset, globals, globalCount);
    /* idx >= 0 guaranteed: lowering only emits IR_GLOBAL_ADDR for valid globals. */
    mfunc_emit(f, MACH_LEA, (MachOperand){ .kind = MO_VREG, .vregId = dst }, (MachOperand){ .kind = MO_GLOBAL, .globalName = globals[idx].name }, (MachOperand){ .kind = MO_NONE });
}

/** @brief IR_ADD/IR_SUB, reusing dst as an operand in place where possible. */
static void isel_select_add_sub(VarMap *ov, FloatSlots *fs, MachFunction *f, const IRInstr *in) {
    if (in->dst.isFloat) {
        isel_select_float_binop(ov, fs, f, in, in->op == IR_ADD ? MACH_ADDSS : MACH_SUBSS);
        return;
    }
    int         dst = varmap_operand_id(ov, in->dst);
    MachOperand lhs = isel_operand_to_mach(&in->src1, ov);
    MachOperand rhs = isel_operand_to_mach(&in->src2, ov);
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
static void isel_select_mul(VarMap *ov, FloatSlots *fs, MachFunction *f, const IRInstr *in) {
    if (in->dst.isFloat) { isel_select_float_binop(ov, fs, f, in, MACH_MULSS); return; }

    int         dst  = varmap_operand_id(ov, in->dst);
    MachOperand src1 = isel_operand_to_mach(&in->src1, ov);
    MachOperand src2 = isel_operand_to_mach(&in->src2, ov);

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
static void isel_select_div_mod(VarMap *ov, FloatSlots *fs, MachFunction *f, const IRInstr *in) {
    if (in->dst.isFloat) { isel_select_float_binop(ov, fs, f, in, MACH_DIVSS); return; }

    int dst = varmap_operand_id(ov, in->dst);
    int lhs = isel_load_operand(&in->src1, ov, f);
    int rhs = isel_load_operand(&in->src2, ov, f);
    mfunc_emit(f, MACH_MOV,  (MachOperand){ .kind = MO_PHYS, .physReg = PHYS_RAX }, (MachOperand){ .kind = MO_VREG, .vregId = lhs }, (MachOperand){ .kind = MO_NONE });
    mfunc_emit(f, MACH_CQO,  (MachOperand){ .kind = MO_NONE }, (MachOperand){ .kind = MO_NONE }, (MachOperand){ .kind = MO_NONE }); // sign-extend RAX into RDX:RAX
    mfunc_emit(f, MACH_IDIV, (MachOperand){ .kind = MO_VREG, .vregId = rhs }, (MachOperand){ .kind = MO_NONE }, (MachOperand){ .kind = MO_NONE });
    // IDIV leaves quotient in RAX, remainder in RDX
    MachPhysReg res = (in->op == IR_DIV) ? PHYS_RAX : PHYS_RDX;
    mfunc_emit(f, MACH_MOV, (MachOperand){ .kind = MO_VREG, .vregId = dst }, (MachOperand){ .kind = MO_PHYS, .physReg = res }, (MachOperand){ .kind = MO_NONE });
}

static void isel_select_neg(VarMap *ov, FloatSlots *fs, MachFunction *f, const IRInstr *in) {
    if (in->dst.isFloat) {
        isel_load_float_into_xmm(&in->src1, ov, fs, f, PHYS_XMM0);
        union { float fl; int i; } u; u.fl = -1.0f;
        int bits = mfunc_new_vreg(f);
        mfunc_emit(f, MACH_MOV, (MachOperand){.kind=MO_VREG,.vregId=bits},
                   (MachOperand){.kind=MO_IMM,.imm=u.i}, (MachOperand){.kind=MO_NONE});
        mfunc_emit(f, MACH_MOVQ_TO_XMM, (MachOperand){.kind=MO_PHYS,.physReg=PHYS_XMM1},
                   (MachOperand){.kind=MO_VREG,.vregId=bits}, (MachOperand){.kind=MO_NONE});
        mfunc_emit(f, MACH_MULSS, (MachOperand){.kind=MO_PHYS,.physReg=PHYS_XMM0},
                   (MachOperand){.kind=MO_PHYS,.physReg=PHYS_XMM1}, (MachOperand){.kind=MO_NONE});
        isel_store_float_from_xmm(&in->dst, ov, fs, f, PHYS_XMM0);
        return;
    }
    int dst = varmap_operand_id(ov, in->dst);
    MachOperand src = isel_operand_to_mach(&in->src1, ov);
    // MACH_NEG is in-place; only copy src into dst first if not already there
    if ((src.kind == MO_VREG ? src.vregId : -1) != dst)
        mfunc_emit(f, MACH_MOV, (MachOperand){ .kind = MO_VREG, .vregId = dst }, src, (MachOperand){ .kind = MO_NONE });
    mfunc_emit(f, MACH_NEG, (MachOperand){ .kind = MO_VREG, .vregId = dst }, (MachOperand){ .kind = MO_NONE }, (MachOperand){ .kind = MO_NONE });
}

static void isel_select_not(VarMap *ov, FloatSlots *fs, MachFunction *f, const IRInstr *in) {
    if (in->src1.isFloat) {
        isel_load_float_into_xmm(&in->src1, ov, fs, f, PHYS_XMM0);
        mfunc_emit(f, MACH_XORPS, (MachOperand){.kind=MO_PHYS,.physReg=PHYS_XMM1},
                   (MachOperand){.kind=MO_PHYS,.physReg=PHYS_XMM1}, (MachOperand){.kind=MO_NONE});
        mfunc_emit(f, MACH_UCOMISS, (MachOperand){.kind=MO_PHYS,.physReg=PHYS_XMM0},
                   (MachOperand){.kind=MO_PHYS,.physReg=PHYS_XMM1}, (MachOperand){.kind=MO_NONE});
        mfunc_emit(f, MACH_SETE, (MachOperand){.kind=MO_PHYS,.physReg=PHYS_AL},
                   (MachOperand){.kind=MO_NONE}, (MachOperand){.kind=MO_NONE});
        mfunc_emit(f, MACH_MOVSX, (MachOperand){.kind=MO_VREG,.vregId=varmap_operand_id(ov,in->dst)},
                   (MachOperand){.kind=MO_PHYS,.physReg=PHYS_AL}, (MachOperand){.kind=MO_NONE});
        return;
    }
    // MOVSX widens the byte result
    int dst = varmap_operand_id(ov, in->dst);
    int sv  = isel_load_operand(&in->src1, ov, f);
    mfunc_emit(f, MACH_TEST,  (MachOperand){ .kind = MO_VREG, .vregId = sv },  (MachOperand){ .kind = MO_VREG, .vregId = sv },  (MachOperand){ .kind = MO_NONE });
    mfunc_emit(f, MACH_SETE,  (MachOperand){ .kind = MO_PHYS, .physReg = PHYS_AL }, (MachOperand){ .kind = MO_NONE }, (MachOperand){ .kind = MO_NONE });
    mfunc_emit(f, MACH_MOVSX, (MachOperand){ .kind = MO_VREG, .vregId = dst }, (MachOperand){ .kind = MO_PHYS, .physReg = PHYS_AL }, (MachOperand){ .kind = MO_NONE });
}

/** @brief Defer comparisons (IR_LT..IR_NE) for possible CMP+Jcc fusion. */
static void isel_select_defer_comparison(VarMap *operandToVreg, PendingCmp *pcmp, const IRInstr *in) {
    int dst = varmap_operand_id(operandToVreg, in->dst);
    pcmp->active  = 1;
    pcmp->instr   = in;
    pcmp->dstVreg = dst;
}

static void isel_select_load_arr(VarMap *ov, FloatSlots *fs, MachFunction *f, const IRInstr *in) {
    if (in->dst.isFloat) {
        int base = varmap_operand_id(ov, in->src1);
        MachOperand memOp;
        if (in->src2.kind == OPND_CONST_INT) {
            int disp = in->src2.data.intVal * 8;
            memOp = (MachOperand){ .kind = MO_MEM, .mem = { .baseVreg = base, .indexVreg = -1, .scale = 0, .disp = disp } };
        } else {
            int idx = isel_load_operand(&in->src2, ov, f);
            memOp = (MachOperand){ .kind = MO_MEM, .mem = { .baseVreg = base, .indexVreg = idx, .scale = 8, .disp = 0 } };
        }
        mfunc_emit(f, MACH_MOVSS, (MachOperand){.kind=MO_PHYS,.physReg=PHYS_XMM0}, memOp, (MachOperand){.kind=MO_NONE});
        isel_store_float_from_xmm(&in->dst, ov, fs, f, PHYS_XMM0);
        return;
    }
    int dst  = varmap_operand_id(ov, in->dst);
    int base = varmap_operand_id(ov, in->src1);
    if (in->src2.kind == OPND_CONST_INT) {
        // constant index folded into the addressing-mode displacement
        int disp = in->src2.data.intVal * 8;
        mfunc_emit(f, MACH_LOAD, (MachOperand){ .kind = MO_VREG, .vregId = dst }, (MachOperand){ .kind = MO_MEM, .mem = { .baseVreg = base, .indexVreg = -1, .scale = 0, .disp = disp } }, (MachOperand){ .kind = MO_NONE });
    } else {
        // dynamic index: real SIB addressing mode (base + idx*8)
        int idx = isel_load_operand(&in->src2, ov, f);
        mfunc_emit(f, MACH_LOAD, (MachOperand){ .kind = MO_VREG, .vregId = dst }, (MachOperand){ .kind = MO_MEM, .mem = { .baseVreg = base, .indexVreg = idx, .scale = 8, .disp = 0 } }, (MachOperand){ .kind = MO_NONE });
    }
}

static void isel_select_store_arr(VarMap *ov, FloatSlots *fs, MachFunction *f, const IRInstr *in) {
    if (in->src2.isFloat || in->src2.kind == OPND_CONST_FLOAT) {
        int base = varmap_operand_id(ov, in->dst);
        isel_load_float_into_xmm(&in->src2, ov, fs, f, PHYS_XMM0);
        MachOperand memOp;
        if (in->src1.kind == OPND_CONST_INT) {
            int disp = in->src1.data.intVal * 8;
            memOp = (MachOperand){ .kind = MO_MEM, .mem = { .baseVreg = base, .indexVreg = -1, .scale = 0, .disp = disp } };
        } else {
            int idx = isel_load_operand(&in->src1, ov, f);
            memOp = (MachOperand){ .kind = MO_MEM, .mem = { .baseVreg = base, .indexVreg = idx, .scale = 8, .disp = 0 } };
        }
        mfunc_emit(f, MACH_MOVSS, memOp, (MachOperand){.kind=MO_PHYS,.physReg=PHYS_XMM0}, (MachOperand){.kind=MO_NONE});
        return;
    }
    int base = varmap_operand_id(ov, in->dst);
    if (in->src1.kind == OPND_CONST_INT) {
        int disp = in->src1.data.intVal * 8;
        int src  = isel_load_operand(&in->src2, ov, f);
        mfunc_emit(f, MACH_STORE, (MachOperand){ .kind = MO_MEM, .mem = { .baseVreg = base, .indexVreg = -1, .scale = 0, .disp = disp } }, (MachOperand){ .kind = MO_VREG, .vregId = src }, (MachOperand){ .kind = MO_NONE });
    } else {
        int idx = isel_load_operand(&in->src1, ov, f);
        int src = isel_load_operand(&in->src2, ov, f);
        mfunc_emit(f, MACH_STORE, (MachOperand){ .kind = MO_MEM, .mem = { .baseVreg = base, .indexVreg = idx, .scale = 8, .disp = 0 } }, (MachOperand){ .kind = MO_VREG, .vregId = src }, (MachOperand){ .kind = MO_NONE });
    }
}

/** @brief Stage one call argument; actual placement happens at IR_CALL. */
static void isel_select_param(const IRInstr *in, PendingArgs *args) {
    if (args->count < MAX_PARAMS) args->ops[args->count++] = in->src1;
}

/** @brief Emit the full call sequence (stack args, register args, CALL, cleanup). */
static void isel_select_call(VarMap *ov, FloatSlots *fs, MachFunction *f,
                         const IRInstr *in, PendingArgs *args) {
    int staged = args->count;
    int arity  = in->src2.kind == OPND_CONST_INT ? (int)in->src2.data.intVal : staged;
    if (arity < 0) arity = 0;
    if (arity > staged) arity = staged;
    int base = staged - arity;
    int intCursor = 0, floatCursor = 0;

    for (int k = 0; k < arity; k++) {
        Operand *op = &args->ops[base + k];
        if (op->isFloat || op->kind == OPND_CONST_FLOAT) {
            if (floatCursor >= 8) { ec_report("instr_selector: >8 argomenti float non supportato\n"); continue; }
            isel_load_float_into_xmm(op, ov, fs, f, PHYS_XMM0 + floatCursor++);
        } else {
            if (intCursor >= NUM_ARG_REGS) { ec_report("instr_selector: >%d argomenti interi non supportato\n", NUM_ARG_REGS); continue; }
            int vreg = isel_load_operand(op, ov, f);
            mfunc_emit(f, MACH_MOV, (MachOperand){.kind=MO_PHYS,.physReg=ARG_REGS[intCursor++]},
                       (MachOperand){.kind=MO_VREG,.vregId=vreg}, (MachOperand){.kind=MO_NONE});
        }
    }
    mfunc_emit(f, MACH_CALL, (MachOperand){.kind=MO_FUNC,.func=in->src1.data.funcName},
               (MachOperand){.kind=MO_NONE}, (MachOperand){.kind=MO_NONE});

    if (in->dst.isFloat) {
        isel_store_float_from_xmm(&in->dst, ov, fs, f, PHYS_XMM0);
    } else {
        int dst = varmap_operand_id(ov, in->dst);
        mfunc_emit(f, MACH_MOV, (MachOperand){.kind=MO_VREG,.vregId=dst},
                   (MachOperand){.kind=MO_PHYS,.physReg=PHYS_RAX}, (MachOperand){.kind=MO_NONE});
    }
    args->count = base;
}

static void isel_select_return(VarMap *ov, FloatSlots *fs, MachFunction *f, const IRInstr *in) {
    if (in->src1.isFloat || in->src1.kind == OPND_CONST_FLOAT) {
        isel_load_float_into_xmm(&in->src1, ov, fs, f, PHYS_XMM0);
    } else {
        int sv = isel_load_operand(&in->src1, ov, f);
        mfunc_emit(f, MACH_MOV, (MachOperand){.kind=MO_PHYS,.physReg=PHYS_RAX},
                   (MachOperand){.kind=MO_VREG,.vregId=sv}, (MachOperand){.kind=MO_NONE});
    }
    mfunc_emit(f, MACH_RET, (MachOperand){.kind=MO_NONE},(MachOperand){.kind=MO_NONE},(MachOperand){.kind=MO_NONE});
}


static MachFunction *isel_select_function(const IRFunction *irf,
                                      const IRGlobalVar *globals,
                                      int globalCount) {
    MachFunction *f = mfunc_create(irf->name);
    g_curLoopDepth = 0; /* reset: must not leak from the previously
                            selected function's last instruction */

    VarMap operandToVreg = varmap_init();
    isel_select_prescan(irf, &operandToVreg, f);

    FloatSlots fs = fslots_create(f->nextVreg);

    mfunc_emit(f, MACH_FUNC_BEGIN, (MachOperand){ .kind = MO_NONE }, (MachOperand){ .kind = MO_NONE }, (MachOperand){ .kind = MO_NONE });
    isel_select_bind_params(irf, &operandToVreg, &fs, f);

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
                isel_flush_pending_cmp(&pcmp, &operandToVreg, &fs, f);
        }

        switch (in->op) {
        case IR_LABEL:       isel_select_label(f, in); break;
        case IR_GOTO:        isel_select_goto(f, in); break;
        case IR_IF_FALSE:    isel_select_if_false(&operandToVreg, &fs, f, &pcmp, in); break;
        case IR_ASSIGN:      isel_select_assign(&operandToVreg, &fs, f, in); break;
        case IR_GLOBAL_ADDR: isel_select_global_addr(&operandToVreg, f, in, globals, globalCount); break;
        case IR_ADD: case IR_SUB: isel_select_add_sub(&operandToVreg, &fs, f, in); break;
        case IR_MUL:          isel_select_mul(&operandToVreg, &fs, f, in); break;
        case IR_DIV: case IR_MOD: isel_select_div_mod(&operandToVreg, &fs, f, in); break;
        case IR_NEG:          isel_select_neg(&operandToVreg, &fs, f, in); break;
        case IR_NOT:           isel_select_not(&operandToVreg, &fs, f, in); break;
        case IR_LT: case IR_LE: case IR_GT: case IR_GE:
        case IR_EQ: case IR_NE: isel_select_defer_comparison(&operandToVreg, &pcmp, in); break;
        case IR_LOAD_ARR:     isel_select_load_arr(&operandToVreg, &fs, f, in); break;
        case IR_STORE_ARR:    isel_select_store_arr(&operandToVreg, &fs, f, in); break;
        case IR_PARAM:        isel_select_param(in, &args); break;
        case IR_CALL:         isel_select_call(&operandToVreg, &fs, f, in, &args); break;
        case IR_RETURN:       isel_select_return(&operandToVreg, &fs, f, in); break;
        }
    }

    // a comparison could be the very last instruction of the function body
    // (e.g. "return a < b;" without an intervening branch): flush it here
    isel_flush_pending_cmp(&pcmp, &operandToVreg, &fs, f);

    // frame size: reserved bytes; regalloc_function continua da qui
    f->frameSize = fs.count * 8;
    fslots_free(&fs);

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
        mp->functions[mp->count++] = isel_select_function(ir->functions[i],
                                                       ir->globals,
                                                       ir->globalCount);
    return mp;
}

/* =========================================================================
 * Assembly printer
 * ========================================================================= */

static void isel_emit_operand(const MachOperand *o, FILE *out) {
    switch (o->kind) {
    case MO_NONE:   break;
    case MO_VREG:   fprintf(out, "%%v%d",      o->vregId);               break;
    case MO_PHYS:
        fprintf(out, "%s", is_xmm_phys(o->physReg) ? phys_name_xmm[o->physReg-PHYS_XMM0]
                                                    : phys_name64[o->physReg]);
        break;
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


/** @brief Emit .bss entries for every zero-initialised global (initCount == 0). */
static void isel_emit_bss_section(FILE *out, const IRProgram *ir) {
    int section_emitted = 0;

    for (int i = 0; i < ir->globalCount; i++) {
        const IRGlobalVar *g = &ir->globals[i];
        if (g->initCount > 0) continue;

        // Emit .bss header lazily only on the first uninitialized global found
        if (!section_emitted) {
            fprintf(out, "\t.bss\n");
            section_emitted = 1;
        }

        int nelems = g->isArray ? g->arraySize : 1;
        fprintf(out, "\t.globl %s\n%s:\n", g->name, g->name);
        fprintf(out, "\t.zero %d\n", nelems * 8); // 8 bytes/element (int and float both stored as 8-byte slots)
    }
} 

/** @brief Emit .data entries for every explicitly-initialised global. */
static void isel_emit_data_section(FILE *out, const IRProgram *ir) {
    int section_emitted = 0;

    for (int i = 0; i < ir->globalCount; i++) {
        const IRGlobalVar *g = &ir->globals[i];
        if (g->initCount == 0) continue;

        // Emit .data header lazily only on the first initialized global found
        if (!section_emitted) {
            fprintf(out, "\t.data\n");
            section_emitted = 1;
        }

        int nelems = g->isArray ? g->arraySize : 1;
        fprintf(out, "\t.globl %s\n%s:\n", g->name, g->name);
        for (int j = 0; j < nelems; j++) {
            // elements past the explicit initializer list default to 0
            long val = (j < g->initCount) ? g->initVals[j] : 0L;
            fprintf(out, "\t.quad %ld\n", val);
        }
    }
}

static void isel_emit_globals(FILE *out, const IRProgram *ir) {
    if (!ir || ir->globalCount == 0) return;
    isel_emit_bss_section(out, ir);
    isel_emit_data_section(out, ir);
}


/**
 * @brief Try to print @p in via one of the fixed/irregular AT&T encodings
 *        that don't fit the generic "mnemonic src, dst" pattern (labels,
 *        prologue/epilogue, single-operand forms, jumps, SETcc — which
 *        prints no operand at all — ...).
 *
 * @param frameSize  Enclosing function's frame size, needed only for the
 *                    MACH_FUNC_BEGIN prologue's `subq`.
 * @return 1 if @p in was fully printed (caller moves to the next
 *         instruction), 0 if it must go through isel_emit_generic_instr().
 */
static int isel_emit_fixed_encoding_instr(const MachInstr *in, int frameSize, FILE *out) {
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
        fprintf(out, "\tidivq\t"); isel_emit_operand(&in->dst, out);
        fprintf(out, "\n"); return 1;
    case MACH_NEG:
        fprintf(out, "\tnegq\t"); isel_emit_operand(&in->dst, out);
        fprintf(out, "\n"); return 1;
    case MACH_NOT:
        fprintf(out, "\tnotq\t"); isel_emit_operand(&in->dst, out);
        fprintf(out, "\n"); return 1;
    case MACH_PUSH:
        fprintf(out, "\tpushq\t"); isel_emit_operand(&in->dst, out);
        fprintf(out, "\n"); return 1;
    case MACH_POP:
        fprintf(out, "\tpopq\t"); isel_emit_operand(&in->dst, out);
        fprintf(out, "\n"); return 1;
    case MACH_CALL:
        fprintf(out, "\tcall\t"); isel_emit_operand(&in->dst, out);
        fprintf(out, "\n"); return 1;
    case MACH_LEA:
        fprintf(out, "\tleaq\t");
        isel_emit_operand(&in->src1, out);
        fprintf(out, ", ");
        isel_emit_operand(&in->dst, out);
        fprintf(out, "\n"); return 1;
    // SETcc variants: fixed destination (%al), no operand printing needed
    case MACH_SETE:  fprintf(out, "\tsete\t%%al\n");  return 1;
    case MACH_SETNE: fprintf(out, "\tsetne\t%%al\n"); return 1;
    case MACH_SETL:  fprintf(out, "\tsetl\t%%al\n");  return 1;
    case MACH_SETLE: fprintf(out, "\tsetle\t%%al\n"); return 1;
    case MACH_SETG:  fprintf(out, "\tsetg\t%%al\n");  return 1;
    case MACH_SETGE: fprintf(out, "\tsetge\t%%al\n"); return 1;
    case MACH_SETB:  fprintf(out, "\tsetb\t%%al\n");  return 1;
    case MACH_SETBE: fprintf(out, "\tsetbe\t%%al\n"); return 1;
    case MACH_SETA:  fprintf(out, "\tseta\t%%al\n");  return 1;
    case MACH_SETAE: fprintf(out, "\tsetae\t%%al\n"); return 1;
    // unconditional/conditional jumps: label operand always in dst
    case MACH_JMP:   fprintf(out, "\tjmp\t.L%d\n",  in->dst.labelId); return 1;
    case MACH_JE:    fprintf(out, "\tje\t.L%d\n",   in->dst.labelId); return 1;
    case MACH_JNE:   fprintf(out, "\tjne\t.L%d\n",  in->dst.labelId); return 1;
    case MACH_JL:    fprintf(out, "\tjl\t.L%d\n",   in->dst.labelId); return 1;
    case MACH_JLE:   fprintf(out, "\tjle\t.L%d\n",  in->dst.labelId); return 1;
    case MACH_JG:    fprintf(out, "\tjg\t.L%d\n",   in->dst.labelId); return 1;
    case MACH_JGE:   fprintf(out, "\tjge\t.L%d\n",  in->dst.labelId); return 1;
    case MACH_JB:    fprintf(out, "\tjb\t.L%d\n",   in->dst.labelId); return 1;
    case MACH_JBE:   fprintf(out, "\tjbe\t.L%d\n",  in->dst.labelId); return 1;
    case MACH_JA:    fprintf(out, "\tja\t.L%d\n",   in->dst.labelId); return 1;
    case MACH_JAE:   fprintf(out, "\tjae\t.L%d\n",  in->dst.labelId); return 1;
    default: return 0; // falls through to the generic two-operand printer
    }
}

/**
 * @brief Print @p in via the generic "mnemonic src, dst" AT&T pattern.
 *
 * Only reached for opcodes isel_emit_fixed_encoding_instr() didn't claim
 * (MOV/MOVSX/ADD/SUB/IMUL/SAL/XOR/CMP/TEST/LOAD/STORE, or "???" defensively).
 */
static void isel_emit_generic_instr(const MachInstr *in, FILE *out) {
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
    case MACH_MOVSS:    mnem = "movss";     break;
    case MACH_ADDSS:    mnem = "addss";     break;
    case MACH_SUBSS:    mnem = "subss";     break;
    case MACH_MULSS:    mnem = "mulss";     break;
    case MACH_DIVSS:    mnem = "divss";     break;
    case MACH_UCOMISS:  mnem = "ucomiss";   break;
    case MACH_XORPS:    mnem = "xorps";     break;
    case MACH_CVTSI2SS: mnem = "cvtsi2ssq"; break;
    case MACH_MOVQ_TO_XMM: mnem = "movq";   break;
    default:         mnem = "???";    break; // should not happen for a well-formed MachInstr stream
    }

    fprintf(out, "\t%s\t", mnem);

    // AT&T operand order is "src, dst". LOAD/STORE address the memory
    // operand via src1/dst depending on direction; other binary ops use
    // src1 as the explicit source (dst is implicitly both input/output,
    // as arranged by the in-place-reuse logic in isel_select_function).
    if (in->op == MACH_LOAD || in->op == MACH_STORE) {
        isel_emit_operand(&in->src1, out);
        fprintf(out, ", ");
        isel_emit_operand(&in->dst, out);
    } else if (in->src2.kind != MO_NONE ) {
        // shouldn't normally happen post-selection, kept defensively
        isel_emit_operand(&in->src1, out);
        fprintf(out, ", ");
        isel_emit_operand(&in->dst, out);
    } else if (in->src1.kind != MO_NONE ) {
        isel_emit_operand(&in->src1, out);
        if (in->dst.kind != MO_NONE) {
            fprintf(out, ", ");
            isel_emit_operand(&in->dst, out);
        }
    } else {
        // single-operand instruction with only dst set
        isel_emit_operand(&in->dst, out);
    }
    fprintf(out, "\n");
}

/** @brief Print every instruction of one function body, in order. */
static void isel_emit_function_body(const MachFunction *f, FILE *out) {
    const int count = f->count;
    const MachInstr *instrs = f->instrs;
    const int frameSize = f->frameSize;

    for (int i = 0; i < count; i++) {
        const MachInstr *in = &instrs[i];
        if (!isel_emit_fixed_encoding_instr(in, frameSize, out)) {
            isel_emit_generic_instr(in, out);
        }
    }
}


void isel_emit_asm(const MachProgram *mp, const IRProgram *ir, FILE *out) {
    isel_emit_globals(out, ir);

    fprintf(out, "\t.text\n");
    for (int fi = 0; fi < mp->count; fi++) {
        const MachFunction *f = mp->functions[fi];
        fprintf(out, "\t.globl %s\n%s:\n", f->name, f->name);
        isel_emit_function_body(f, out);
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