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

/* =========================================================================
 * Physical register name table
 * ========================================================================= */

// indexed by MachPhysReg enum value; last entry ("%al") is the 8-bit
// alias of PHYS_RAX, used only when printing SETcc destinations
static const char *phys_name64[] = {
    "%rax", "%rcx", "%rdx", "%rsi", "%rdi",
    "%r8",  "%r9",  "%r10", "%r11",
    "%rbx", "%r12", "%r13", "%r14", "%r15",
    "%rbp", "%rsp", "%al"
};

/* =========================================================================
 * MachOperand constructors
 * ========================================================================= */

// Small factory helpers: each builds a MachOperand tagged with the right
// kind. Kept as separate one-liners so call sites read like `mo_imm(5)`
// instead of repeating compound-literal syntax everywhere.
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

static inline void mfunc_emit(MachFunction *f, MachOp op,
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

/* =========================================================================
 * VarMap bridge
 * ========================================================================= */

// thin rename: VarMap ids double as vreg ids in this backend (1:1 mapping,
// no separate allocation step needed)
static inline int operand_to_vreg(const Operand *op, VarMap *vm) {
    return varmap_operand_id(vm, *op);
}

/**
 * @brief Materialise an IR operand into a vreg, emitting a load if it is
 *        a literal constant.
 *
 * Unlike operand_to_mach(), this ALWAYS returns a vreg id (never an
 * immediate MachOperand). Used where the target machine instruction has
 * no immediate-operand form (e.g. IDIV divisor, PUSH source) and the
 * value must sit in a register regardless of how it was expressed in IR.
 */
static int load_operand(const Operand *op, VarMap *vm, MachFunction *f) {
    switch (op->kind) {
    case OPND_VAR:
    case OPND_TEMP:
        // already a storage location: just resolve its vreg id
        return operand_to_vreg(op, vm);
    case OPND_CONST_INT: {
        // materialise the constant with a MOV into a fresh vreg
        int dst = mfunc_new_vreg(f);
        mfunc_emit(f, MACH_MOV, mo_vreg(dst), mo_imm(op->data.intVal), mo_none());
        return dst;
    }
    case OPND_CONST_FLOAT: {
        // float constants are moved as their raw bit pattern (no SSE support
        // in this backend); reinterpret via union to avoid UB from casting
        // float->long directly, which would convert the VALUE not the bits
        int dst = mfunc_new_vreg(f);
        union { float fl; int i; } u; u.fl = op->data.floatVal;
        mfunc_emit(f, MACH_MOV, mo_vreg(dst), mo_imm(u.i), mo_none());
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
 */
static inline MachOperand operand_to_mach(const Operand *op, VarMap *vm,
                                           MachFunction *mf) {
    (void)mf;
    switch (op->kind) {
    case OPND_CONST_INT:   return mo_imm(op->data.intVal);
    case OPND_CONST_FLOAT: {
        // same bit-reinterpretation trick as load_operand()
        union { float f; int i; } u; u.f = op->data.floatVal;
        return mo_imm(u.i);
    }
    case OPND_VAR:
    case OPND_TEMP:        return mo_vreg(operand_to_vreg(op, vm));
    default:               return mo_none();
    }
}

/* =========================================================================
 * Global name lookup — unica funzione rimasta per globali.
 * Serve solo nel case IR_GLOBAL_ADDR per risolvere symOffset -> nome simbolo.
 * ========================================================================= */

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
static inline MachOp comparison_to_setcc(IROp cmpOp) {
    switch (cmpOp) {
    case IR_LT: return MACH_SETL;  case IR_LE: return MACH_SETLE;
    case IR_GT: return MACH_SETG;  case IR_GE: return MACH_SETGE;
    case IR_EQ: return MACH_SETE;  case IR_NE: return MACH_SETNE;
    default:    return MACH_SETE;
    }
}

// System V AMD64 ABI: first 6 integer/pointer arguments go in these
// registers, in this exact order; anything beyond NUM_ARG_REGS is spilled
// to the stack by the caller (see IR_CALL / IR_PARAM handling below)
static const MachPhysReg ARG_REGS[] = {
    PHYS_RDI, PHYS_RSI, PHYS_RDX, PHYS_RCX, PHYS_R8, PHYS_R9
};

/* =========================================================================
 * PendingCmp — deferred comparison for CMP+Jcc fusion
 * ========================================================================= */

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

/**
 * @brief Force-materialise a pending comparison as CMP + SETcc + MOVSX.
 *
 * Called whenever the next instruction turns out NOT to be a matching
 * IR_IF_FALSE (so fusion is not possible) — the boolean value must be
 * computed the "normal" way after all.
 */
static void flush_pending_cmp(PendingCmp *pcmp, VarMap *vm, MachFunction *f) {
    if (!pcmp->active) return;

    const IRInstr *ci = pcmp->instr;
    MachOperand lhs   = operand_to_mach(&ci->src1, vm, f);
    MachOperand rhs   = operand_to_mach(&ci->src2, vm, f);
    IROp cmpOp        = ci->op;

    // x86 CMP requires the first operand to be a register; if the IR gave
    // us an immediate on the left, swap operands and flip the comparison
    // sense so the encoded instruction stays valid and semantically equal
    if (lhs.kind == MO_IMM && rhs.kind != MO_IMM) {
        MachOperand t = lhs; lhs = rhs; rhs = t;
        cmpOp = flip_cmp(cmpOp);
    }
    // both operands immediate (rare, e.g. after partial constant folding):
    // materialise the left one into a register so CMP has a valid encoding
    if (lhs.kind == MO_IMM) {
        int tmp = mfunc_new_vreg(f);
        mfunc_emit(f, MACH_MOV, mo_vreg(tmp), lhs, mo_none());
        lhs = mo_vreg(tmp);
    }

    // CMP sets flags; SETcc reads flags into %al; MOVSX sign-extends the
    // 1-byte 0/1 result into the full 64-bit destination vreg
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
    g_curLoopDepth = 0; /* reset: non deve trapelare dall'ultima istruzione
                           della funzione precedentemente selezionata */

    /* Phase 1: pre-scan — assegna vreg id a tutti gli operandi IR.
     * Include anche i parametri formali (irf->params), anche quelli MAI
     * usati nel corpo: se non li registrassimo qui, un eventuale primo
     * uso post-sync (fase 2) creerebbe un id fuori sincronia con
     * f->nextVreg, facendolo collidere con un temporaneo isel-interno. */
    VarMap vm;
    varmap_init(&vm);
    // walk every instruction once just to register every distinct
    // variable/temp with a stable vreg id BEFORE any code is emitted
    for (int i = 0; i < irf->count; i++) {
        const IRInstr *in = &irf->instrs[i];
        varmap_operand_id(&vm, in->dst);
        varmap_operand_id(&vm, in->src1);
        varmap_operand_id(&vm, in->src2);
    }
    // also register formal parameters even if unused in the body, so their
    // ids stay dense and don't get silently skipped
    for (int p = 0; p < irf->paramCount; p++)
        varmap_operand_id(&vm, irf->params[p]);

    /* Phase 2: sincronizza nextVreg per evitare collisioni con temp isel-interni. */
    // any fresh vreg allocated later (mfunc_new_vreg) must start counting
    // AFTER every id already handed out by VarMap above
    f->nextVreg = vm.nextId;

    /* Phase 3: selezione istruzioni, passata singola. */
    mfunc_emit(f, MACH_FUNC_BEGIN, mo_none(), mo_none(), mo_none());

    /* Phase 3-bis: binding parametri formali <- registri ABI (System V:
     * rdi,rsi,rdx,rcx,r8,r9). Senza questo MOV il vreg del parametro non
     * riceve mai il valore passato dal chiamante. Parametri oltre il 6°
     * (passati su stack dal chiamante) non sono ancora supportati: non
     * esiste nel modello attuale un modo pulito per esprimere un indirizzo
     * MO_MEM a offset positivo da %rbp (MO_STACK e' cablato per offset
     * negativi negli spill, vedi emit_operand); segnalato e basta finche'
     * non serve davvero. */
    for (int p = 0; p < irf->paramCount; p++) {
        int vreg = operand_to_vreg(&irf->params[p], &vm);
        if (p < NUM_ARG_REGS) {
            // copy incoming ABI register into the parameter's vreg
            mfunc_emit(f, MACH_MOV, mo_vreg(vreg), mo_phys(ARG_REGS[p]), mo_none());
        } else {
            // stack-passed parameters (7th onward) are not implemented yet
            fprintf(stderr,
                    "instr_selector: parametro #%d di '%s' passato su stack "
                    "(>%d parametri) non ancora supportato\n",
                    p + 1, irf->name, NUM_ARG_REGS);
        }
    }

    int param_vregs[MAX_PARAMS], param_count = 0; // pending IR_PARAM args, flushed by IR_CALL
    PendingCmp pcmp = { .active = 0 };

    for (int i = 0; i < irf->count; i++) {
        const IRInstr *in = &irf->instrs[i];
        g_curLoopDepth = in->loopDepth;

        /* Flush comparazione differita se l'istruzione corrente non puo' fondersi. */
        if (pcmp.active) {
            int must_materialize = 1;
            // fusion only applies when THIS instruction is the IF_FALSE
            // that consumes exactly the pending comparison's result vreg
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
                /* Fusione CMP + Jcc. */
                // the pending comparison's result is consumed directly by
                // this branch: skip SETcc/MOVSX entirely and emit CMP + Jcc
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

                // IR_IF_FALSE jumps when the condition is FALSE, so the
                // emitted Jcc must test the NEGATED comparison (e.g.
                // "if_false a<b" -> jump when a>=b, i.e. MACH_JGE)
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
                // no fusable comparison pending: condition is an ordinary
                // 0/1-valued vreg, test it directly with TEST+JE
                mfunc_emit(f, MACH_TEST, mo_vreg(cond_vreg), mo_vreg(cond_vreg), mo_none());
                mfunc_emit(f, MACH_JE,   mo_label(lbl), mo_none(), mo_none());
            }
            break;
        }

        /* ---- Assignment ---- */
        case IR_ASSIGN: {
            int         dst = operand_to_vreg(&in->dst, &vm);
            MachOperand src = operand_to_mach(&in->src1, &vm, f);
            mfunc_emit(f, MACH_MOV, mo_vreg(dst), src, mo_none());
            break;
        }

        /* ---- Global address: lowering gia' fatto in IR, qui solo LEA ---- */
        case IR_GLOBAL_ADDR: {
            int dst = operand_to_vreg(&in->dst, &vm);
            int idx = find_global_idx(in->src1.data.globalOffset, globals, globalCount);
            /* idx >= 0 garantito: il lowering emette IR_GLOBAL_ADDR solo per globali validi. */
            mfunc_emit(f, MACH_LEA, mo_vreg(dst), mo_global(globals[idx].name), mo_none());
            break;
        }

        /* ---- Arithmetic: ADD / SUB ---- */
        case IR_ADD:
        case IR_SUB: {
            int         dst = operand_to_vreg(&in->dst,  &vm);
            MachOperand lhs = operand_to_mach(&in->src1, &vm, f);
            MachOperand rhs = operand_to_mach(&in->src2, &vm, f);
            MachOp      mop = (in->op == IR_ADD) ? MACH_ADD : MACH_SUB;
            int lhs_id = (lhs.kind == MO_VREG) ? lhs.vregId : -1;
            int rhs_id = (rhs.kind == MO_VREG) ? rhs.vregId : -1;

            // x86 ADD/SUB are two-operand (dst is also an implicit source),
            // so we try to reuse dst as one of the operands in place to
            // avoid an extra MOV; four cases handled below:
            if (lhs_id == dst) {
                // dst already holds lhs: "dst += rhs" (or -=) directly
                mfunc_emit(f, mop, mo_vreg(dst), rhs, mo_none());
            } else if (in->op == IR_ADD && rhs_id == dst) {
                // addition is commutative: dst already holds rhs, "dst += lhs"
                mfunc_emit(f, MACH_ADD, mo_vreg(dst), lhs, mo_none());
            } else if (in->op == IR_SUB && rhs_id == dst) {
                // subtraction is NOT commutative: dst holds rhs but we need
                // lhs - dst, so negate dst then add lhs: -(dst) + lhs == lhs - dst
                mfunc_emit(f, MACH_NEG, mo_vreg(dst), mo_none(), mo_none());
                mfunc_emit(f, MACH_ADD, mo_vreg(dst), lhs, mo_none());
            } else {
                // general case: neither operand aliases dst, seed it first
                mfunc_emit(f, MACH_MOV, mo_vreg(dst), lhs, mo_none());
                mfunc_emit(f, mop,      mo_vreg(dst), rhs, mo_none());
            }
            break;
        }

        /* ---- Arithmetic: MUL ---- */
        case IR_MUL: {
            int         dst  = operand_to_vreg(&in->dst,  &vm);
            MachOperand src1 = operand_to_mach(&in->src1, &vm, f);
            MachOperand src2 = operand_to_mach(&in->src2, &vm, f);

            // identify which side (if any) is the immediate operand, so the
            // power-of-2 check below always looks at imm_side regardless of
            // which IR operand position it originally occupied
            MachOperand reg_side = src1, imm_side = src2;
            if (src1.kind == MO_IMM && src2.kind != MO_IMM) {
                reg_side = src2; imm_side = src1;
            }

            if (imm_side.kind == MO_IMM && imm_side.imm > 0 &&
                (imm_side.imm & (imm_side.imm - 1)) == 0) {
                /* Potenza di 2 -> shift sinistro. */
                // classic strength reduction: n*2^k == n << k; cheaper than
                // IMUL on most x86 microarchitectures
                int shift = 0; long v = imm_side.imm;
                while (v > 1) { shift++; v >>= 1; }
                int reg_id = (reg_side.kind == MO_VREG) ? reg_side.vregId : -1;
                if (reg_id != dst)
                    mfunc_emit(f, MACH_MOV, mo_vreg(dst), reg_side, mo_none());
                mfunc_emit(f, MACH_SAL, mo_vreg(dst), mo_imm(shift), mo_none());
            } else {
                // general IMUL path: same dst-aliasing trick as ADD/SUB above,
                // but MUL is commutative so both src1==dst and src2==dst cases
                // can reuse dst without extra bookkeeping
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
            // x86 IDIV takes a 128-bit dividend in RDX:RAX and a single
            // register divisor; both operands must be forced into registers
            // (load_operand, not operand_to_mach) since IDIV has no
            // immediate-operand form
            int dst = operand_to_vreg(&in->dst, &vm);
            int lhs = load_operand(&in->src1, &vm, f);
            int rhs = load_operand(&in->src2, &vm, f);
            mfunc_emit(f, MACH_MOV,  mo_phys(PHYS_RAX), mo_vreg(lhs), mo_none());
            mfunc_emit(f, MACH_CQO,  mo_none(), mo_none(), mo_none()); // sign-extend RAX into RDX:RAX
            mfunc_emit(f, MACH_IDIV, mo_vreg(rhs), mo_none(), mo_none());
            // IDIV leaves quotient in RAX, remainder in RDX
            MachPhysReg res = (in->op == IR_DIV) ? PHYS_RAX : PHYS_RDX;
            mfunc_emit(f, MACH_MOV, mo_vreg(dst), mo_phys(res), mo_none());
            break;
        }

        /* ---- Unary: NEG ---- */
        case IR_NEG: {
            int         dst = operand_to_vreg(&in->dst,  &vm);
            MachOperand src = operand_to_mach(&in->src1, &vm, f);
            // MACH_NEG is in-place (negates dst itself); only copy src into
            // dst first if it isn't already sitting there
            if ((src.kind == MO_VREG ? src.vregId : -1) != dst)
                mfunc_emit(f, MACH_MOV, mo_vreg(dst), src, mo_none());
            mfunc_emit(f, MACH_NEG, mo_vreg(dst), mo_none(), mo_none());
            break;
        }

        /* ---- Unary: NOT ---- */
        case IR_NOT: {
            // logical NOT: result is 1 iff operand == 0. TEST sv,sv sets ZF
            // when sv is zero; SETE captures that into %al; MOVSX widens it.
            int dst = operand_to_vreg(&in->dst, &vm);
            int sv  = load_operand(&in->src1, &vm, f);
            mfunc_emit(f, MACH_TEST,  mo_vreg(sv),  mo_vreg(sv),  mo_none());
            mfunc_emit(f, MACH_SETE,  mo_phys(PHYS_AL), mo_none(), mo_none());
            mfunc_emit(f, MACH_MOVSX, mo_vreg(dst), mo_phys(PHYS_AL), mo_none());
            break;
        }

        /* ---- Comparisons: deferred per CMP+Jcc fusion ---- */
        case IR_LT: case IR_LE: case IR_GT: case IR_GE:
        case IR_EQ: case IR_NE: {
            /* Dopo il lowering, src1/src2 sono gia' temp normali (mai OPND_GLOBAL).
             * Nessun branch speciale necessario: flush_pending_cmp usa
             * operand_to_mach che gestisce temp e costanti correttamente. */
            // don't emit anything yet: just remember this comparison so the
            // next loop iteration can decide fusion vs. full materialisation
            int dst = operand_to_vreg(&in->dst, &vm);
            pcmp.active  = 1;
            pcmp.instr   = in;
            pcmp.dstVreg = dst;
            break;
        }

        /* ---- Array load ---- */
/* ---- Array load ---- */
case IR_LOAD_ARR: {
    int dst  = operand_to_vreg(&in->dst,  &vm);
    int base = operand_to_vreg(&in->src1, &vm);
    if (in->src2.kind == OPND_CONST_INT) {
        /* indice costante → displacement, niente registro indice
         * scalare globale: (%base) invece di mov $0,%r; (%base,%r,8) */
        // constant index: fold it into the addressing-mode displacement
        // (elements are 8 bytes wide) instead of materialising an index
        // register that would always hold the same value
        int disp = in->src2.data.intVal * 8;
        mfunc_emit(f, MACH_LOAD, mo_vreg(dst),
                   mo_mem(base, -1, 0, disp), mo_none());
    } else {
        // dynamic index: needs a real SIB addressing mode (base + idx*8)
        int idx = load_operand(&in->src2, &vm, f);
        mfunc_emit(f, MACH_LOAD, mo_vreg(dst),
                   mo_mem(base, idx, 8, 0), mo_none());
    }
    break;
}

/* ---- Array store ---- */
case IR_STORE_ARR: {
    int base = operand_to_vreg(&in->dst, &vm);
    if (in->src1.kind == OPND_CONST_INT) {
        /* indice costante → displacement, niente registro indice */
        // same constant-index optimisation as IR_LOAD_ARR above
        int disp = in->src1.data.intVal * 8;
        int src  = load_operand(&in->src2, &vm, f);
        mfunc_emit(f, MACH_STORE,
                   mo_mem(base, -1, 0, disp), mo_vreg(src), mo_none());
    } else {
        int idx = load_operand(&in->src1, &vm, f);
        int src = load_operand(&in->src2, &vm, f);
        mfunc_emit(f, MACH_STORE,
                   mo_mem(base, idx, 8, 0), mo_vreg(src), mo_none());
    }
    break;
}

        /* ---- Function call arguments ---- */
        case IR_PARAM: {
            // collect argument vregs; the actual register/stack placement
            // happens later, in bulk, when IR_CALL is reached (arity is
            // only known once all IR_PARAM instructions preceding the
            // call have been seen)
            MachOperand sv = operand_to_mach(&in->src1, &vm, f);
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
            // arguments beyond the 6 ABI registers are pushed on the stack,
            // in REVERSE order (rightmost argument pushed first) so they
            // end up in left-to-right order in memory once the callee reads them
            for (int k = n - 1; k >= NUM_ARG_REGS; k--)
                mfunc_emit(f, MACH_PUSH, mo_vreg(param_vregs[k]),
                           mo_none(), mo_none());
            // first up-to-6 arguments go into the fixed ABI registers
            int reg_args = (n < NUM_ARG_REGS) ? n : NUM_ARG_REGS;
            for (int k = 0; k < reg_args; k++)
                mfunc_emit(f, MACH_MOV,
                           mo_phys(ARG_REGS[k]), mo_vreg(param_vregs[k]),
                           mo_none());
            mfunc_emit(f, MACH_CALL, mo_func(in->src1.data.funcName),
                       mo_none(), mo_none());
            // caller cleans up any stack-pushed arguments after the call
            // returns (cdecl-style stack discipline for the overflow args)
            int extra = n - NUM_ARG_REGS;
            if (extra > 0) {
                int adj = mfunc_new_vreg(f);
                mfunc_emit(f, MACH_MOV, mo_vreg(adj), mo_imm(extra * 8L), mo_none());
                mfunc_emit(f, MACH_ADD, mo_phys(PHYS_RSP), mo_vreg(adj), mo_none());
            }
            // return value convention: RAX -> destination vreg
            int dst = operand_to_vreg(&in->dst, &vm);
            mfunc_emit(f, MACH_MOV, mo_vreg(dst), mo_phys(PHYS_RAX), mo_none());
            param_count = 0; // reset staging buffer for the next call site
            break;
        }

        /* ---- Return ---- */
        case IR_RETURN: {
            int sv = load_operand(&in->src1, &vm, f);
            mfunc_emit(f, MACH_MOV, mo_phys(PHYS_RAX), mo_vreg(sv), mo_none());
            mfunc_emit(f, MACH_RET, mo_none(), mo_none(), mo_none());
            break;
        }

        } /* switch */
    } /* for each IR instruction */

    // a comparison could be the very last instruction of the function body
    // (e.g. "return a < b;" without an intervening branch): flush it here
    // so its result actually gets materialised into a register
    flush_pending_cmp(&pcmp, &vm, f);

    // frame size: 8 bytes per vreg slot, rounded up to the 16-byte x86-64
    // stack alignment required at call boundaries
    int raw      = f->nextVreg * 8;
    f->frameSize = (raw + 15) & ~15;

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
 * Public API — isel_emit_asm
 * ========================================================================= */

void isel_emit_asm(const MachProgram *mp, const IRProgram *ir, FILE *out) {

    /* --- Global variable declarations --- */
    if (ir && ir->globalCount > 0) {
        // .bss holds zero-initialised globals (no data to store in the
        // binary, just reserve space); split from .data below
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
                fprintf(out, "\t.zero %d\n", nelems * 8); // 8 bytes/element (int and float both stored as 8-byte slots)
            }
        }

        // .data holds globals with an explicit initializer list
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
                    // elements past the explicit initializer list default to 0
                    // (C semantics: partial array initializers zero-fill the rest)
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

            // first switch: opcodes with irregular/fixed-text encodings that
            // don't fit the generic "mnemonic src, dst" pattern handled below
            switch (in->op) {
            case MACH_LABEL:
                fprintf(out, ".L%d:\n", in->dst.labelId); continue;
            case MACH_FUNC_BEGIN:
                // standard x86-64 prologue: save caller's frame pointer,
                // establish new frame, reserve local storage
                fprintf(out, "\tpushq\t%%rbp\n");
                fprintf(out, "\tmovq\t%%rsp, %%rbp\n");
                if (f->frameSize > 0)
                    fprintf(out, "\tsubq\t$%d, %%rsp\n", f->frameSize);
                continue;
            case MACH_RET:
                // `leave` restores rsp/rbp in one instruction, equivalent to
                // "movq %rbp,%rsp; popq %rbp"
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
                fprintf(out, "\tleaq\t");
                emit_operand(&in->src1, out);
                fprintf(out, ", ");
                emit_operand(&in->dst, out);
                fprintf(out, "\n"); continue;
            // SETcc variants: fixed destination (%al), no operand printing needed
            case MACH_SETE:  fprintf(out, "\tsete\t%%al\n");  continue;
            case MACH_SETNE: fprintf(out, "\tsetne\t%%al\n"); continue;
            case MACH_SETL:  fprintf(out, "\tsetl\t%%al\n");  continue;
            case MACH_SETLE: fprintf(out, "\tsetle\t%%al\n"); continue;
            case MACH_SETG:  fprintf(out, "\tsetg\t%%al\n");  continue;
            case MACH_SETGE: fprintf(out, "\tsetge\t%%al\n"); continue;
            // unconditional/conditional jumps: label operand always in dst
            case MACH_JMP:   fprintf(out, "\tjmp\t.L%d\n",  in->dst.labelId); continue;
            case MACH_JE:    fprintf(out, "\tje\t.L%d\n",   in->dst.labelId); continue;
            case MACH_JNE:   fprintf(out, "\tjne\t.L%d\n",  in->dst.labelId); continue;
            case MACH_JL:    fprintf(out, "\tjl\t.L%d\n",   in->dst.labelId); continue;
            case MACH_JLE:   fprintf(out, "\tjle\t.L%d\n",  in->dst.labelId); continue;
            case MACH_JG:    fprintf(out, "\tjg\t.L%d\n",   in->dst.labelId); continue;
            case MACH_JGE:   fprintf(out, "\tjge\t.L%d\n",  in->dst.labelId); continue;
            default: break; // falls through to the generic two-operand printer below
            }

            /* Two-operand instructions. */
            // generic path: pick the AT&T mnemonic, then decide operand
            // order/count based on which operands are populated
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

            // AT&T operand order is "src, dst". LOAD/STORE address the
            // memory operand via src1/dst depending on direction; other
            // binary ops use src1 as the explicit source (dst is
            // implicitly both an input and output, as arranged in
            // select_function's in-place-reuse logic above).
            if (in->op == MACH_LOAD || in->op == MACH_STORE) {
                emit_operand(&in->src1, out);
                fprintf(out, ", ");
                emit_operand(&in->dst, out);
            } else if (in->src2.kind != MO_NONE) {
                // shouldn't normally happen post-selection (all our binary
                // ops are 2-operand dst/src1 forms), kept defensively
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
                // single-operand instruction with only dst set
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