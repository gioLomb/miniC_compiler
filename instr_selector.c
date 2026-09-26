#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "instr_selector.h"
#include "varmap.h"
#include "parser/errorCollector.h"


/* Selection only — assembly printing lives in isel_emit.c */

// System V AMD64 ABI: first 6 integer/pointer arguments go in these
// registers, in this exact order; anything beyond NUM_ARG_REGS is spilled
// to the stack by the caller
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


static MachFunction *mfunc_create(const char *name) {
    MachFunction *f = calloc(1, sizeof(MachFunction));
    f->name      = name;
    f->capacity  = 64;
    f->instrs    = malloc((size_t)f->capacity * sizeof(MachInstr));
    f->nextVreg  = 0;
    f->fNextVreg = 0;
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
static inline int mfunc_new_freg(MachFunction *f) { return f->fNextVreg++; }

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
        /* Materialise float bits as an integer immediate (bit pattern, not value). */
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


static int isel_find_global_idx(int symOff,
                            const IRGlobalVar *globals, int globalCount) {
    for (int i = 0; i < globalCount; i++)
        if (globals[i].symOffset == symOff) return i;
    return -1;
}


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
// its boolean result into %al (signed integer comparisons)
static inline MachOpCode isel_comparison_to_setcc(IROp cmpOp) {
    switch (cmpOp) {
    case IR_LT: return MACH_SETL;  case IR_LE: return MACH_SETLE;
    case IR_GT: return MACH_SETG;  case IR_GE: return MACH_SETGE;
    case IR_EQ: return MACH_SETE;  case IR_NE: return MACH_SETNE;
    default:    return MACH_SETE;
    }
}

/* UCOMISS sets CF/ZF (not SF/OF); use unsigned SETcc for float compares. */
static inline MachOpCode isel_comparison_to_setcc_unsigned(IROp cmpOp) {
    switch (cmpOp) {
    case IR_LT: return MACH_SETB;  case IR_LE: return MACH_SETBE;
    case IR_GT: return MACH_SETA;  case IR_GE: return MACH_SETAE;
    case IR_EQ: return MACH_SETE;  case IR_NE: return MACH_SETNE;
    default:    return MACH_SETE;
    }
}


/* =========================================================================
 * Float virtual-register map
 * ========================================================================= */

/**
 * Maps a VarMap operand-identity id to a dense float vreg id (MO_VREG_F).
 * Float values enter the Chaitin-Briggs pipeline exactly like integers.
 */
typedef struct {
    int *fvregOfId; /**< fvregOfId[varmapId] = float vreg id, -1 = unassigned. */
} FloatVregMap;

static FloatVregMap fvmap_create(int cap) {
    FloatVregMap m;
    m.fvregOfId = malloc((size_t)(cap > 0 ? cap : 1) * sizeof(int));
    memset(m.fvregOfId, -1, (size_t)(cap > 0 ? cap : 1) * sizeof(int));
    return m;
}
static void fvmap_free(FloatVregMap *m) {
    free(m->fvregOfId);
    m->fvregOfId = NULL;
}

static MachOperand fvmap_operand(FloatVregMap *m, MachFunction *f, int varmapId) {
    if (m->fvregOfId[varmapId] < 0)
        m->fvregOfId[varmapId] = mfunc_new_freg(f);
    return (MachOperand){ .kind = MO_VREG_F, .vregId = m->fvregOfId[varmapId] };
}


static int isel_load_float_bits(const Operand *op, MachFunction *f) {
    union { float fl; int i; } u; u.fl = op->data.floatVal;
    int tmp = mfunc_new_vreg(f);
    mfunc_emit(f, MACH_MOV, (MachOperand){.kind=MO_VREG,.vregId=tmp},
               (MachOperand){.kind=MO_IMM,.imm=u.i}, (MachOperand){.kind=MO_NONE});
    return tmp;
}

/** Materialise any float IR operand (var/temp/const) into an MO_VREG_F. */
static MachOperand isel_float_operand(const Operand *op, VarMap *ov, FloatVregMap *fvm,
                                      MachFunction *f) {
    if (op->kind == OPND_CONST_FLOAT) {
        int bits = isel_load_float_bits(op, f); /* int scratch with raw bits */
        int fv   = mfunc_new_freg(f);
        mfunc_emit(f, MACH_MOVQ_TO_XMM,
                   (MachOperand){ .kind = MO_VREG_F, .vregId = fv },
                   (MachOperand){ .kind = MO_VREG,   .vregId = bits },
                   (MachOperand){ .kind = MO_NONE });
        return (MachOperand){ .kind = MO_VREG_F, .vregId = fv };
    }
    int id = varmap_operand_id(ov, *op);
    return fvmap_operand(fvm, f, id);
}

/** RMW float binop: dst = src1 OP src2 via copy-then-in-place SSE op. */
static void isel_select_float_binop(VarMap *ov, FloatVregMap *fvm, MachFunction *f,
                                    const IRInstr *in, MachOpCode sseOp) {
    MachOperand lhs = isel_float_operand(&in->src1, ov, fvm, f);
    MachOperand rhs = isel_float_operand(&in->src2, ov, fvm, f);
    MachOperand dst = fvmap_operand(fvm, f, varmap_operand_id(ov, in->dst));

    if (!(lhs.kind == MO_VREG_F && lhs.vregId == dst.vregId))
        mfunc_emit(f, MACH_MOVSS, dst, lhs, (MachOperand){ .kind = MO_NONE });
    mfunc_emit(f, sseOp, dst, rhs, (MachOperand){ .kind = MO_NONE });
}



/**
 * @brief Force-materialise a pending comparison as CMP + SETcc + MOVSX.
 *
 * Called whenever the next instruction turns out NOT to be a matching
 * IR_IF_FALSE (so fusion is not possible) — the boolean value must be
 * computed the "normal" way after all.
 */
static void isel_flush_pending_cmp(PendingCmp *pcmp, VarMap *operandToVreg, FloatVregMap *fvm, MachFunction *f) {
    if (!pcmp->active) return;

    const IRInstr *ci = pcmp->instr;

    if (ci->src1.isFloat || ci->src2.isFloat ||
        ci->src1.kind == OPND_CONST_FLOAT || ci->src2.kind == OPND_CONST_FLOAT) {
        MachOperand a = isel_float_operand(&ci->src1, operandToVreg, fvm, f);
        MachOperand b = isel_float_operand(&ci->src2, operandToVreg, fvm, f);
        mfunc_emit(f, MACH_UCOMISS, a, b, (MachOperand){.kind=MO_NONE});
        mfunc_emit(f, isel_comparison_to_setcc_unsigned(ci->op),
                   (MachOperand){.kind=MO_PHYS,.physReg=PHYS_AL},
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
    f->nextVreg = varmap_count(operandToVreg);
}

/**
 * @brief Phase 3-bis of isel_select_function: bind formal parameters to their
 *        ABI registers (System V: rdi,rsi,rdx,rcx,r8,r9).
 *
 * Parameters beyond the 6th (passed on the caller's stack) are not yet
 * supported — flagged with a diagnostic rather than silently miscompiled.
 */
static void isel_select_bind_params(const IRFunction *irf, VarMap *ov, FloatVregMap *fvm, MachFunction *f) {
    int intCursor = 0, floatCursor = 0;
    for (int p = 0; p < irf->paramCount; p++) {
        Operand *param = &irf->params[p];
        if (param->isFloat) {
            if (floatCursor >= 8) { ec_report("instr_selector: >8 parametri float non supportato\n"); continue; }
            MachOperand dst = fvmap_operand(fvm, f, varmap_operand_id(ov, *param));
            mfunc_emit(f, MACH_MOVSS, dst,
                       (MachOperand){.kind=MO_PHYS,.physReg=PHYS_XMM0 + floatCursor++},
                       (MachOperand){.kind=MO_NONE});
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
/**
 * @brief Per-function index: for every temp id read as a source operand
 *        (src1/src2) anywhere in the function, how many times it is read
 *        and the earliest instruction index of such a read.
 *
 * Replaces the previous isel_cmp_result_used_later() linear rescan
 * (O(n) per call, O(n^2) worst case across an if-chain-heavy function,
 * e.g. the scFn* examples in stress_test.c) with a single O(n) build
 * shared by every comparison in the function, then O(1) queries.
 * Direct-address map keyed by tempId (offset by the minimum tempId
 * seen), same pattern as cp.c's build_label_to_instr().
 *
 * NOTE: the replaced function also matched tempId against an
 * instruction's dst operand (a defensive "redefinition" check). Every
 * temp id in this front-end is allocated once (ir_alloc_temp_id() /
 * nextTemp++) and written by exactly one instruction, so a second dst
 * match for the same tempId can never occur; that branch was dead code
 * and is intentionally dropped here.
 */
typedef struct {
    int  minTemp;    /**< Smallest tempId seen as a source operand, or 0 if none. */
    int  size;       /**< Number of slots (0 if the function reads no temps). */
    int *useCount;   /**< useCount[tempId - minTemp]: read-use count.       */
    int *firstUse;   /**< firstUse[tempId - minTemp]: earliest read index, or -1. */
} TempUseIndex;

static void isel_index_temp_src(const Operand *op, int minT, int maxT,
                                int instrIdx, int *useCount, int *firstUse) {
    if (op->kind != OPND_TEMP) return;
    int t = op->data.tempId;
    if (t < minT || t > maxT) return; /* defensive; range already covers every src temp */
    int slot = t - minT;
    useCount[slot]++;
    if (firstUse[slot] < 0) firstUse[slot] = instrIdx;
}

/**
 * @brief Build the source-use index for @p irf (see TempUseIndex doc).
 *
 * Two linear passes: first finds the [minT, maxT] range of temp ids ever
 * read as src1/src2, then fills useCount[]/firstUse[] over that range.
 * Caller releases the arrays with isel_free_temp_use_index().
 */
static TempUseIndex isel_build_temp_use_index(const IRFunction *irf) {
    TempUseIndex idx = { 0, 0, NULL, NULL };
    int minT = INT_MAX, maxT = INT_MIN;

    // first pass: find the [minT, maxT] range of temp ids used as sources
    for (int i = 0; i < irf->count; i++) {
        const IRInstr *in = &irf->instrs[i];
        if (in->src1.kind == OPND_TEMP) {
            if (in->src1.data.tempId < minT) minT = in->src1.data.tempId;
            if (in->src1.data.tempId > maxT) maxT = in->src1.data.tempId;
        }
        if (in->src2.kind == OPND_TEMP) {
            if (in->src2.data.tempId < minT) minT = in->src2.data.tempId;
            if (in->src2.data.tempId > maxT) maxT = in->src2.data.tempId;
        }
    }
    if (minT > maxT) return idx; // no temp ever read as a source: empty index

    idx.minTemp  = minT;
    idx.size     = maxT - minT + 1;
    idx.useCount = malloc((size_t)idx.size * sizeof(int));
    idx.firstUse = malloc((size_t)idx.size * sizeof(int));
    memset(idx.useCount, 0,    (size_t)idx.size * sizeof(int));
    memset(idx.firstUse, 0xFF, (size_t)idx.size * sizeof(int)); // -1 sentinel (two's complement)

    // second pass: fill counts/first-use indices for every source occurrence
    for (int i = 0; i < irf->count; i++) {
        const IRInstr *in = &irf->instrs[i];
        isel_index_temp_src(&in->src1, minT, maxT, i, idx.useCount, idx.firstUse);
        isel_index_temp_src(&in->src2, minT, maxT, i, idx.useCount, idx.firstUse);
    }
    return idx;
}

/** @brief Release the arrays owned by a TempUseIndex built via isel_build_temp_use_index(). */
static inline void isel_free_temp_use_index(TempUseIndex *idx) {
    free(idx->useCount);
    free(idx->firstUse);
    idx->useCount = idx->firstUse = NULL;
}

/**
 * @brief O(1) replacement for the old isel_cmp_result_used_later() scan:
 *        true if @p tempId is read anywhere in the function other than
 *        at instruction @p excludeIdx (the fusing IF_FALSE).
 */
static inline int isel_temp_used_elsewhere(const TempUseIndex *idx, int tempId, int excludeIdx) {
    if (tempId < 0) return 1; /* conservative, mirrors original tempId<0 case */
    int slot = tempId - idx->minTemp;
    if (idx->size == 0 || slot < 0 || slot >= idx->size) return 0; // never read as a source
    int cnt = idx->useCount[slot];
    if (cnt == 0) return 0;
    if (cnt >= 2) return 1;
    return idx->firstUse[slot] != excludeIdx; // single read: elsewhere iff not the excluded one
}

static void isel_select_if_false(VarMap *operandToVreg, FloatVregMap *fvm, MachFunction *f, PendingCmp *pcmp,
                             const IRInstr *in, const IRFunction *irf, int ifIdx,
                             const TempUseIndex *tempUses) {
    int cond_vreg = varmap_operand_id(operandToVreg, in->src1);
    int lbl       = in->dst.data.labelId;

    /* Only fuse when the boolean is not needed after the branch. */
    int canFuse = pcmp->active && cond_vreg == pcmp->dstVreg;
    if (canFuse && pcmp->instr && pcmp->instr->dst.kind == OPND_TEMP) {
        if (isel_temp_used_elsewhere(tempUses, pcmp->instr->dst.data.tempId, ifIdx))
            canFuse = 0;
    }

    if (canFuse) {
        /* CMP + Jcc fusion: skip SETcc/MOVSX entirely. */
        const IRInstr *comparisonInstr = pcmp->instr;

        if (comparisonInstr->src1.isFloat) {
            MachOperand a = isel_float_operand(&comparisonInstr->src1, operandToVreg, fvm, f);
            MachOperand b = isel_float_operand(&comparisonInstr->src2, operandToVreg, fvm, f);
            mfunc_emit(f, MACH_UCOMISS, a, b, (MachOperand){.kind=MO_NONE});
            MachOpCode jcc;
            switch (comparisonInstr->op) {
            case IR_LT: jcc = MACH_JAE; break; case IR_LE: jcc = MACH_JA;  break;
            case IR_GT: jcc = MACH_JBE; break; case IR_GE: jcc = MACH_JB;  break;
            case IR_EQ: jcc = MACH_JNE; break; case IR_NE: jcc = MACH_JE;  break;
            default:    jcc = MACH_JMP; break;
            }
            mfunc_emit(f, jcc, (MachOperand){ .kind = MO_LABEL, .labelId = lbl }, (MachOperand){ .kind = MO_NONE }, (MachOperand){ .kind = MO_NONE });
            pcmp->active = 0;
            return;
        }

        MachOperand lhs   = isel_operand_to_mach(&comparisonInstr->src1, operandToVreg);
        MachOperand rhs   = isel_operand_to_mach(&comparisonInstr->src2, operandToVreg);
        IROp cmpOp        = comparisonInstr->op;

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

static void isel_select_assign(VarMap *ov, FloatVregMap *fvm, MachFunction *f, const IRInstr *in) {
    if (in->dst.isFloat) {
        MachOperand src;
        if (in->src1.isFloat || in->src1.kind == OPND_CONST_FLOAT) {
            src = isel_float_operand(&in->src1, ov, fvm, f);
        } else {
            int srcVreg = isel_load_operand(&in->src1, ov, f);
            int tmp = mfunc_new_freg(f);
            mfunc_emit(f, MACH_CVTSI2SS, (MachOperand){.kind=MO_VREG_F,.vregId=tmp},
                       (MachOperand){.kind=MO_VREG,.vregId=srcVreg}, (MachOperand){.kind=MO_NONE});
            src = (MachOperand){.kind=MO_VREG_F,.vregId=tmp};
        }
        MachOperand dst = fvmap_operand(fvm, f, varmap_operand_id(ov, in->dst));
        mfunc_emit(f, MACH_MOVSS, dst, src, (MachOperand){.kind=MO_NONE});
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
static void isel_select_add_sub(VarMap *ov, FloatVregMap *fvm, MachFunction *f, const IRInstr *in) {
    if (in->dst.isFloat) {
        isel_select_float_binop(ov, fvm, f, in, in->op == IR_ADD ? MACH_ADDSS : MACH_SUBSS);
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
static void isel_select_mul(VarMap *ov, FloatVregMap *fvm, MachFunction *f, const IRInstr *in) {
    if (in->dst.isFloat) { isel_select_float_binop(ov, fvm, f, in, MACH_MULSS); return; }

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
static void isel_select_div_mod(VarMap *ov, FloatVregMap *fvm, MachFunction *f, const IRInstr *in) {
    if (in->dst.isFloat) { isel_select_float_binop(ov, fvm, f, in, MACH_DIVSS); return; }

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

static void isel_select_neg(VarMap *ov, FloatVregMap *fvm, MachFunction *f, const IRInstr *in) {
    if (in->dst.isFloat) {
        MachOperand src = isel_float_operand(&in->src1, ov, fvm, f);
        Operand neg1 = { .kind = OPND_CONST_FLOAT, .isFloat = 1, .data.floatVal = -1.0f };
        MachOperand m1 = isel_float_operand(&neg1, ov, fvm, f);
        MachOperand dst = fvmap_operand(fvm, f, varmap_operand_id(ov, in->dst));
        if (!(src.kind == MO_VREG_F && src.vregId == dst.vregId))
            mfunc_emit(f, MACH_MOVSS, dst, src, (MachOperand){.kind=MO_NONE});
        mfunc_emit(f, MACH_MULSS, dst, m1, (MachOperand){.kind=MO_NONE});
        return;
    }
    int dst = varmap_operand_id(ov, in->dst);
    MachOperand src = isel_operand_to_mach(&in->src1, ov);
    // MACH_NEG is in-place; only copy src into dst first if not already there
    if ((src.kind == MO_VREG ? src.vregId : -1) != dst)
        mfunc_emit(f, MACH_MOV, (MachOperand){ .kind = MO_VREG, .vregId = dst }, src, (MachOperand){ .kind = MO_NONE });
    mfunc_emit(f, MACH_NEG, (MachOperand){ .kind = MO_VREG, .vregId = dst }, (MachOperand){ .kind = MO_NONE }, (MachOperand){ .kind = MO_NONE });
}

static void isel_select_not(VarMap *ov, FloatVregMap *fvm, MachFunction *f, const IRInstr *in) {
    if (in->src1.isFloat) {
        MachOperand v = isel_float_operand(&in->src1, ov, fvm, f);
        Operand zero = { .kind = OPND_CONST_FLOAT, .isFloat = 1, .data.floatVal = 0.0f };
        MachOperand z = isel_float_operand(&zero, ov, fvm, f);
        mfunc_emit(f, MACH_UCOMISS, v, z, (MachOperand){.kind=MO_NONE});
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
static inline void isel_select_defer_comparison(VarMap *operandToVreg, PendingCmp *pcmp, const IRInstr *in) {
    int dst = varmap_operand_id(operandToVreg, in->dst);
    pcmp->active  = 1;
    pcmp->instr   = in;
    pcmp->dstVreg = dst;
}

static void isel_select_load_arr(VarMap *ov, FloatVregMap *fvm, MachFunction *f, const IRInstr *in) {
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
        MachOperand dst = fvmap_operand(fvm, f, varmap_operand_id(ov, in->dst));
        mfunc_emit(f, MACH_MOVSS, dst, memOp, (MachOperand){.kind=MO_NONE});
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

static void isel_select_store_arr(VarMap *ov, FloatVregMap *fvm, MachFunction *f, const IRInstr *in) {
    if (in->src2.isFloat || in->src2.kind == OPND_CONST_FLOAT) {
        int base = varmap_operand_id(ov, in->dst);
        MachOperand val = isel_float_operand(&in->src2, ov, fvm, f);
        MachOperand memOp;
        if (in->src1.kind == OPND_CONST_INT) {
            int disp = in->src1.data.intVal * 8;
            memOp = (MachOperand){ .kind = MO_MEM, .mem = { .baseVreg = base, .indexVreg = -1, .scale = 0, .disp = disp } };
        } else {
            int idx = isel_load_operand(&in->src1, ov, f);
            memOp = (MachOperand){ .kind = MO_MEM, .mem = { .baseVreg = base, .indexVreg = idx, .scale = 8, .disp = 0 } };
        }
        mfunc_emit(f, MACH_MOVSS, memOp, val, (MachOperand){.kind=MO_NONE});
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
static const IRFunction *isel_find_function(const IRProgram *prog, const char *name) {
    if (!prog || !name) return NULL;
    for (int i = 0; i < prog->count; i++)
        if (prog->functions[i] && prog->functions[i]->name &&
            strcmp(prog->functions[i]->name, name) == 0)
            return prog->functions[i];
    return NULL;
}

static void isel_select_call(VarMap *ov, FloatVregMap *fvm, MachFunction *f,
                         const IRInstr *in, PendingArgs *args, const IRProgram *prog) {
    int staged = args->count;
    int arity  = in->src2.kind == OPND_CONST_INT ? (int)in->src2.data.intVal : staged;
    if (arity < 0) arity = 0;
    if (arity > staged) arity = staged;
    int base = staged - arity;
    int intCursor = 0, floatCursor = 0;

    const IRFunction *callee = isel_find_function(prog, in->src1.data.funcName);

    for (int k = 0; k < arity; k++) {
        Operand *op = &args->ops[base + k];
        // Prefer formal parameter type when available (handles int→float
        // widening at the call site as required by C semantics).
        int formalIsFloat = 0;
        if (callee && k < callee->paramCount)
            formalIsFloat = callee->params[k].isFloat;

        if (formalIsFloat || op->isFloat || op->kind == OPND_CONST_FLOAT) {
            if (floatCursor >= 8) { ec_report("instr_selector: >8 argomenti float non supportato\n"); continue; }
            if (op->isFloat || op->kind == OPND_CONST_FLOAT) {
                MachOperand val = isel_float_operand(op, ov, fvm, f);
                mfunc_emit(f, MACH_MOVSS,
                           (MachOperand){.kind=MO_PHYS,.physReg=PHYS_XMM0 + floatCursor},
                           val, (MachOperand){.kind=MO_NONE});
            } else {
                // int → float conversion
                int vreg = isel_load_operand(op, ov, f);
                mfunc_emit(f, MACH_CVTSI2SS, (MachOperand){.kind=MO_PHYS,.physReg=PHYS_XMM0 + floatCursor},
                           (MachOperand){.kind=MO_VREG,.vregId=vreg}, (MachOperand){.kind=MO_NONE});
            }
            floatCursor++;
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
        MachOperand dst = fvmap_operand(fvm, f, varmap_operand_id(ov, in->dst));
        mfunc_emit(f, MACH_MOVSS, dst,
                   (MachOperand){.kind=MO_PHYS,.physReg=PHYS_XMM0},
                   (MachOperand){.kind=MO_NONE});
    } else {
        int dst = varmap_operand_id(ov, in->dst);
        mfunc_emit(f, MACH_MOV, (MachOperand){.kind=MO_VREG,.vregId=dst},
                   (MachOperand){.kind=MO_PHYS,.physReg=PHYS_RAX}, (MachOperand){.kind=MO_NONE});
    }
    args->count = base;
}

static void isel_select_return(VarMap *ov, FloatVregMap *fvm, MachFunction *f, const IRInstr *in) {
    if (in->src1.isFloat || in->src1.kind == OPND_CONST_FLOAT) {
        MachOperand v = isel_float_operand(&in->src1, ov, fvm, f);
        mfunc_emit(f, MACH_MOVSS, (MachOperand){.kind=MO_PHYS,.physReg=PHYS_XMM0},
                   v, (MachOperand){.kind=MO_NONE});
    } else {
        int sv = isel_load_operand(&in->src1, ov, f);
        mfunc_emit(f, MACH_MOV, (MachOperand){.kind=MO_PHYS,.physReg=PHYS_RAX},
                   (MachOperand){.kind=MO_VREG,.vregId=sv}, (MachOperand){.kind=MO_NONE});
    }
    mfunc_emit(f, MACH_RET, (MachOperand){.kind=MO_NONE},(MachOperand){.kind=MO_NONE},(MachOperand){.kind=MO_NONE});
}


static MachFunction *isel_select_function(const IRFunction *irf,
                                      const IRGlobalVar *globals,
                                      int globalCount,
                                      const IRProgram *prog) {
    MachFunction *f = mfunc_create(irf->name);
    g_curLoopDepth = 0; /* per-function reset */

    VarMap *operandToVreg = varmap_create();
    isel_select_prescan(irf, operandToVreg, f);

    int nIds = varmap_count(operandToVreg);
    FloatVregMap fvm = fvmap_create(nIds > 0 ? nIds : 1);

    mfunc_emit(f, MACH_FUNC_BEGIN, (MachOperand){ .kind = MO_NONE }, (MachOperand){ .kind = MO_NONE }, (MachOperand){ .kind = MO_NONE });
    isel_select_bind_params(irf, operandToVreg, &fvm, f);

    PendingArgs args = { .count = 0 };
    PendingCmp  pcmp = { .active = 0 };
    TempUseIndex tempUses = isel_build_temp_use_index(irf);

    for (int i = 0; i < irf->count; i++) {
        const IRInstr *in = &irf->instrs[i];
        g_curLoopDepth = in->loopDepth;

        // flush a deferred comparison if the current instruction can't
        // fuse with it (wrong cond, or comparison result still needed later)
        if (pcmp.active) {
            int must_materialize = 1;
            if (in->op == IR_IF_FALSE &&
                varmap_operand_id(operandToVreg, in->src1) == pcmp.dstVreg) {
                must_materialize = 0;
                if (pcmp.instr && pcmp.instr->dst.kind == OPND_TEMP) {
                    if (isel_temp_used_elsewhere(&tempUses, pcmp.instr->dst.data.tempId, i))
                        must_materialize = 1;
                }
            }
            if (must_materialize)
                isel_flush_pending_cmp(&pcmp, operandToVreg, &fvm, f);
        }

        switch (in->op) {
        case IR_LABEL:       isel_select_label(f, in); break;
        case IR_GOTO:        isel_select_goto(f, in); break;
        case IR_IF_FALSE:    isel_select_if_false(operandToVreg, &fvm, f, &pcmp, in, irf, i, &tempUses); break;
        case IR_ASSIGN:      isel_select_assign(operandToVreg, &fvm, f, in); break;
        case IR_ITOF: {
            int srcVreg = isel_load_operand(&in->src1, operandToVreg, f);
            MachOperand dst = fvmap_operand(&fvm, f, varmap_operand_id(operandToVreg, in->dst));
            mfunc_emit(f, MACH_CVTSI2SS, dst,
                       (MachOperand){ .kind = MO_VREG, .vregId = srcVreg },
                       (MachOperand){ .kind = MO_NONE });
            break;
        }
        case IR_GLOBAL_ADDR: isel_select_global_addr(operandToVreg, f, in, globals, globalCount); break;
        case IR_ADD: case IR_SUB: isel_select_add_sub(operandToVreg, &fvm, f, in); break;
        case IR_MUL:          isel_select_mul(operandToVreg, &fvm, f, in); break;
        case IR_DIV: case IR_MOD: isel_select_div_mod(operandToVreg, &fvm, f, in); break;
        case IR_NEG:          isel_select_neg(operandToVreg, &fvm, f, in); break;
        case IR_NOT:           isel_select_not(operandToVreg, &fvm, f, in); break;
        case IR_LT: case IR_LE: case IR_GT: case IR_GE:
        case IR_EQ: case IR_NE: isel_select_defer_comparison(operandToVreg, &pcmp, in); break;
        case IR_LOAD_ARR:     isel_select_load_arr(operandToVreg, &fvm, f, in); break;
        case IR_STORE_ARR:    isel_select_store_arr(operandToVreg, &fvm, f, in); break;
        case IR_PARAM:        isel_select_param(in, &args); break;
        case IR_CALL:         isel_select_call(operandToVreg, &fvm, f, in, &args, prog); break;
        case IR_RETURN:       isel_select_return(operandToVreg, &fvm, f, in); break;
        }
    }


    isel_flush_pending_cmp(&pcmp, operandToVreg, &fvm, f);
    isel_free_temp_use_index(&tempUses);
    fvmap_free(&fvm);
    varmap_destroy(operandToVreg);
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
                                                       ir->globalCount,
                                                       ir);
    return mp;
}