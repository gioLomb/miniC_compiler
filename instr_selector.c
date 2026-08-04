#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "instr_selector.h"

/* =========================================================================
 * Nomi registri fisici — ordine identico all'enum MachPhysReg.
 * Usato da emit_operand (pre-regalloc: stampa %vN; post-regalloc: fisico)
 * e da regalloc per MO_MEM dopo la colorazione.
 * ========================================================================= */
static const char *phys_name64[] = {
    "%rax", "%rcx", "%rdx", "%rsi", "%rdi",
    "%r8",  "%r9",  "%r10", "%r11",
    "%rbx", "%r12", "%r13", "%r14", "%r15",
    "%rbp", "%rsp", "%al"
};

/* =========================================================================
 * Costruttori operandi.
 * ========================================================================= */
static inline MachOperand mo_none(void) {
    MachOperand o; o.kind = MO_NONE; return o;
}
static inline MachOperand mo_vreg(int id) {
    MachOperand o; o.kind = MO_VREG; o.vregId = id; return o;
}
static inline MachOperand mo_phys(MachPhysReg r) {
    MachOperand o; o.kind = MO_PHYS; o.physReg = (int)r; return o;
}
static inline MachOperand mo_imm(long v) {
    MachOperand o; o.kind = MO_IMM; o.imm = v; return o;
}
static inline MachOperand mo_label(int id) {
    MachOperand o; o.kind = MO_LABEL; o.labelId = id; return o;
}
static inline MachOperand mo_func(const char *name) {
    MachOperand o; o.kind = MO_FUNC; o.func = name; return o;
}
static inline MachOperand mo_mem(int base, int index, int scale, int disp) {
    MachOperand o;
    o.kind = MO_MEM;
    o.mem.baseVreg  = base;
    o.mem.indexVreg = index;
    o.mem.scale     = scale;
    o.mem.disp      = disp;
    return o;
}

/* =========================================================================
 * loopDepth corrente durante la selezione: propagato a ogni MachInstr
 * emessa, cosi' regalloc puo' pesare il costo di spill senza ricalcolare
 * l'annidamento dal CFG macchina.
 * ========================================================================= */
static int g_curLoopDepth = 0;

/* =========================================================================
 * MachFunction helpers.
 * ========================================================================= */
static MachFunction *mfunc_create(const char *name) {
    MachFunction *f = calloc(1, sizeof(MachFunction));
    f->name     = name;
    f->capacity = 64;
    f->instrs   = malloc((size_t)f->capacity * sizeof(MachInstr));
    f->nextVreg = 0;
    return f;
}

static void mfunc_emit(MachFunction *f, MachOp op,
                        MachOperand dst, MachOperand src1, MachOperand src2) {
    if (f->count == f->capacity) {
        f->capacity *= 2;
        f->instrs = realloc(f->instrs, (size_t)f->capacity * sizeof(MachInstr));
    }
    MachInstr *in = &f->instrs[f->count++];
    in->op        = op;
    in->dst       = dst;
    in->src1      = src1;
    in->src2      = src2;
    in->scale     = 8;
    in->loopDepth = g_curLoopDepth;   /* propaga depth dall'IRInstr corrente */
}

static inline int mfunc_new_vreg(MachFunction *f) { return f->nextVreg++; }

/* =========================================================================
 * VarMap: Operand IR → virtual register ID (array lineare, max 1024).
 * ========================================================================= */
#define VARMAP_MAX 1024

typedef struct { int kind, a, b, vregId; } VMapEntry;
typedef struct { VMapEntry entries[VARMAP_MAX]; int count; } VarMap;

static int vmap_lookup(VarMap *vm, int kind, int a, int b) {
    for (int i = 0; i < vm->count; i++) {
        VMapEntry *e = &vm->entries[i];
        if (e->kind == kind && e->a == a && e->b == b) return e->vregId;
    }
    return -1;
}
static int vmap_get_or_create(VarMap *vm, MachFunction *f,
                               int kind, int a, int b) {
    int id = vmap_lookup(vm, kind, a, b);
    if (id >= 0) return id;
    id = mfunc_new_vreg(f);
    if (vm->count < VARMAP_MAX) {
        VMapEntry *e = &vm->entries[vm->count++];
        e->kind = kind; e->a = a; e->b = b; e->vregId = id;
    }
    return id;
}
static int operand_to_vreg(const Operand *op, VarMap *vm, MachFunction *f) {
    if (op->kind == OPND_VAR)
        return vmap_get_or_create(vm, f, 0, op->data.varLevel, op->data.varOffset);
    if (op->kind == OPND_TEMP)
        return vmap_get_or_create(vm, f, 1, op->data.tempId, 0);
    return -1;
}

/* =========================================================================
 * Caricamento Operand IR → vreg (emette MOV se necessario).
 * ========================================================================= */
static int load_operand(const Operand *op, VarMap *vm, MachFunction *f) {
    switch (op->kind) {
    case OPND_VAR:
    case OPND_TEMP:
        return operand_to_vreg(op, vm, f);
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

static MachOperand operand_to_mach(const Operand *op, VarMap *vm,
                                    MachFunction *mf) {
    switch (op->kind) {
    case OPND_CONST_INT:   return mo_imm(op->data.intVal);
    case OPND_CONST_FLOAT: { union { float f; int i; } u; u.f = op->data.floatVal; return mo_imm(u.i); }
    case OPND_VAR:
    case OPND_TEMP:        return mo_vreg(operand_to_vreg(op, vm, mf));
    default:               return mo_none();
    }
}

static IROp flip_cmp(IROp op) {
    switch (op) {
    case IR_LT: return IR_GT; case IR_GT: return IR_LT;
    case IR_LE: return IR_GE; case IR_GE: return IR_LE;
    default:    return op;
    }
}

static MachOp comparison_to_setcc(IROp cmpOp) {
    switch (cmpOp) {
    case IR_LT: return MACH_SETL;  case IR_LE: return MACH_SETLE;
    case IR_GT: return MACH_SETG;  case IR_GE: return MACH_SETGE;
    case IR_EQ: return MACH_SETE;  case IR_NE: return MACH_SETNE;
    default:    return MACH_SETE;
    }
}

/* Convenzione chiamata System V AMD64 */
static const MachPhysReg ARG_REGS[] = {
    PHYS_RDI, PHYS_RSI, PHYS_RDX, PHYS_RCX, PHYS_R8, PHYS_R9
};
#define NUM_ARG_REGS 6
#define MAX_PARAMS   64

/* =========================================================================
 * Selezione istruzioni per singola funzione.
 * ========================================================================= */
typedef struct { int active; const IRInstr *instr; int dstVreg; } PendingCmp;

static MachFunction *select_function(const IRFunction *irf) {
    MachFunction *f = mfunc_create(irf->name);
    VarMap vm; vm.count = 0;

    mfunc_emit(f, MACH_FUNC_BEGIN, mo_none(), mo_none(), mo_none());

    int param_vregs[MAX_PARAMS], param_count = 0;
    PendingCmp pcmp; pcmp.active = 0;

    for (int i = 0; i < irf->count; i++) {
        const IRInstr *in = &irf->instrs[i];
        g_curLoopDepth = in->loopDepth;   /* propaga depth per MachInstr generate */

        /* --- flush eventuale CMP/TEST pendente se non fuso con IF_FALSE --- */
        if (pcmp.active) {
            int must_materialize = 1;
            if (in->op == IR_IF_FALSE)
                must_materialize = (operand_to_vreg(&in->src1, &vm, f) != pcmp.dstVreg);

            if (must_materialize) {
                const IRInstr *ci = pcmp.instr;
                MachOperand lhs = operand_to_mach(&ci->src1, &vm, f);
                MachOperand rhs = operand_to_mach(&ci->src2, &vm, f);
                IROp cmpOp = ci->op;
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
                mfunc_emit(f, MACH_MOVSX, mo_vreg(pcmp.dstVreg), mo_phys(PHYS_AL), mo_none());
                pcmp.active = 0;
            }
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
            int cond_vreg = operand_to_vreg(&in->src1, &vm, f);
            int lbl       = in->dst.data.labelId;

            if (pcmp.active && cond_vreg == pcmp.dstVreg) {
                /* CMP/TEST + IF_FALSE → CMP + Jcc fusi */
                const IRInstr *ci = pcmp.instr;
                MachOperand lhs = operand_to_mach(&ci->src1, &vm, f);
                MachOperand rhs = operand_to_mach(&ci->src2, &vm, f);
                IROp cmpOp = ci->op;
                if (lhs.kind == MO_IMM && rhs.kind != MO_IMM) {
                    MachOperand t = lhs; lhs = rhs; rhs = t; cmpOp = flip_cmp(cmpOp);
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

        case IR_ASSIGN: {
            int dst         = operand_to_vreg(&in->dst, &vm, f);
            MachOperand src = operand_to_mach(&in->src1, &vm, f);
            mfunc_emit(f, MACH_MOV, mo_vreg(dst), src, mo_none());
            break;
        }

        case IR_ADD:
        case IR_SUB: {
            int         dst = operand_to_vreg(&in->dst,  &vm, f);
            MachOperand lhs = operand_to_mach(&in->src1, &vm, f);
            MachOperand rhs = operand_to_mach(&in->src2, &vm, f);
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

        case IR_MUL: {
            int         dst  = operand_to_vreg(&in->dst,  &vm, f);
            MachOperand src1 = operand_to_mach(&in->src1, &vm, f);
            MachOperand src2 = operand_to_mach(&in->src2, &vm, f);
            MachOperand reg_side = src1, imm_side = src2;
            if (src1.kind == MO_IMM && src2.kind != MO_IMM) {
                reg_side = src2; imm_side = src1;
            }
            /* peephole: moltiplicazione per potenza di 2 → shift */
            if (imm_side.kind == MO_IMM && imm_side.imm > 0 &&
                (imm_side.imm & (imm_side.imm - 1)) == 0) {
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

        case IR_DIV:
        case IR_MOD: {
            int dst = operand_to_vreg(&in->dst, &vm, f);
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
            int         dst = operand_to_vreg(&in->dst,  &vm, f);
            MachOperand src = operand_to_mach(&in->src1, &vm, f);
            if ((src.kind == MO_VREG ? src.vregId : -1) != dst)
                mfunc_emit(f, MACH_MOV, mo_vreg(dst), src, mo_none());
            mfunc_emit(f, MACH_NEG, mo_vreg(dst), mo_none(), mo_none());
            break;
        }

        case IR_NOT: {
            int dst = operand_to_vreg(&in->dst, &vm, f);
            int src = load_operand(&in->src1, &vm, f);
            mfunc_emit(f, MACH_TEST,  mo_vreg(src), mo_vreg(src), mo_none());
            mfunc_emit(f, MACH_SETE,  mo_phys(PHYS_AL), mo_none(), mo_none());
            mfunc_emit(f, MACH_MOVSX, mo_vreg(dst), mo_phys(PHYS_AL), mo_none());
            break;
        }

        case IR_LT: case IR_LE: case IR_GT: case IR_GE:
        case IR_EQ: case IR_NE: {
            int dst = operand_to_vreg(&in->dst, &vm, f);
            pcmp.active  = 1;
            pcmp.instr   = in;
            pcmp.dstVreg = dst;
            break;
        }

        case IR_LOAD_ARR: {
            int dst  = operand_to_vreg(&in->dst,  &vm, f);
            int base = operand_to_vreg(&in->src1, &vm, f);
            int idx  = load_operand(&in->src2, &vm, f);
            mfunc_emit(f, MACH_LOAD, mo_vreg(dst),
                       mo_mem(base, idx, 8, 0), mo_none());
            break;
        }

        case IR_STORE_ARR: {
            int base = operand_to_vreg(&in->dst, &vm, f);
            int idx  = load_operand(&in->src1, &vm, f);
            int src  = load_operand(&in->src2, &vm, f);
            mfunc_emit(f, MACH_STORE,
                       mo_mem(base, idx, 8, 0), mo_vreg(src), mo_none());
            break;
        }

        case IR_PARAM: {
            MachOperand src_mo = operand_to_mach(&in->src1, &vm, f);
            int src;
            if (src_mo.kind == MO_IMM) {
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
            /* argomenti in eccesso su stack (ordine inverso) */
            for (int k = n - 1; k >= NUM_ARG_REGS; k--)
                mfunc_emit(f, MACH_PUSH, mo_vreg(param_vregs[k]),
                           mo_none(), mo_none());
            /* primi argomenti nei registri fisici */
            int reg_args = (n < NUM_ARG_REGS) ? n : NUM_ARG_REGS;
            for (int k = 0; k < reg_args; k++)
                mfunc_emit(f, MACH_MOV,
                           mo_phys(ARG_REGS[k]), mo_vreg(param_vregs[k]),
                           mo_none());
            mfunc_emit(f, MACH_CALL, mo_func(in->src1.data.funcName),
                       mo_none(), mo_none());
            /* pulizia argomenti in eccesso */
            int extra = n - NUM_ARG_REGS;
            if (extra > 0) {
                int adj = mfunc_new_vreg(f);
                mfunc_emit(f, MACH_MOV, mo_vreg(adj), mo_imm(extra * 8L), mo_none());
                mfunc_emit(f, MACH_ADD, mo_phys(PHYS_RSP), mo_vreg(adj), mo_none());
            }
            int dst = operand_to_vreg(&in->dst, &vm, f);
            mfunc_emit(f, MACH_MOV, mo_vreg(dst), mo_phys(PHYS_RAX), mo_none());
            param_count = 0;
            break;
        }

        case IR_RETURN: {
            int src = load_operand(&in->src1, &vm, f);
            mfunc_emit(f, MACH_MOV, mo_phys(PHYS_RAX), mo_vreg(src), mo_none());
            mfunc_emit(f, MACH_RET, mo_none(), mo_none(), mo_none());
            break;
        }

        } /* switch */
    } /* for */

    /* flush CMP/TEST pendente alla fine della funzione (caso degenere) */
    if (pcmp.active) {
        const IRInstr *ci = pcmp.instr;
        MachOperand lhs = operand_to_mach(&ci->src1, &vm, f);
        MachOperand rhs = operand_to_mach(&ci->src2, &vm, f);
        IROp cmpOp = ci->op;
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
        mfunc_emit(f, MACH_MOVSX, mo_vreg(pcmp.dstVreg), mo_phys(PHYS_AL), mo_none());
    }

    int raw = f->nextVreg * 8;
    f->frameSize = (raw + 15) & ~15;
    return f;
}

/* =========================================================================
 * isel_select: entry point.
 * ========================================================================= */
MachProgram *isel_select(const IRProgram *ir) {
    MachProgram *mp = calloc(1, sizeof(MachProgram));
    mp->capacity  = ir->count ? ir->count : 1;
    mp->functions = malloc((size_t)mp->capacity * sizeof(MachFunction *));
    for (int i = 0; i < ir->count; i++)
        mp->functions[mp->count++] = select_function(ir->functions[i]);
    return mp;
}

/* =========================================================================
 * isel_emit_asm: assembly AT&T x86-64.
 * ========================================================================= */
static void emit_operand(const MachOperand *o, FILE *out) {
    switch (o->kind) {
    case MO_NONE:  break;
    case MO_VREG:  fprintf(out, "%%v%d", o->vregId);              break;
    case MO_PHYS:  fprintf(out, "%s",    phys_name64[o->physReg]); break;
    case MO_IMM:   fprintf(out, "$%ld",  o->imm);                 break;
    case MO_LABEL: fprintf(out, ".L%d",  o->labelId);             break;
    case MO_FUNC:  fprintf(out, "%s",    o->func);                break;
    case MO_STACK:
        fprintf(out, "-%d(%%rbp)", o->stackOff);
        break;
    case MO_MEM:
        /* post-regalloc: baseVreg/indexVreg sono MachPhysReg */
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

void isel_emit_asm(const MachProgram *mp, FILE *out) {
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
            if (in->op == MACH_LOAD) {
                emit_operand(&in->src1, out);
                fprintf(out, ", ");
                emit_operand(&in->dst, out);
            } else if (in->op == MACH_STORE) {
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
 * mach_free.
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