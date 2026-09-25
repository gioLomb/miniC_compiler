/**
 * @file isel_emit.c
 * @brief AT&T x86-64 assembly printer for post-regalloc MachProgram.
 *
 * Split from instr_selector.c for readability; no instruction-selection logic.
 */

#include "instr_selector.h"
#include <stdio.h>
#include <stdlib.h>

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


/* =========================================================================
 * Assembly printer
 * ========================================================================= */

static void isel_emit_operand(const MachOperand *o, FILE *out) {
    switch (o->kind) {
    case MO_NONE:   break;
    case MO_VREG:   fprintf(out, "%%v%d",      o->vregId);               break;
    case MO_VREG_F: fprintf(out, "%%fv%d",     o->vregId);               break;
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
    /* Mark stack non-executable (silences linker warnings on modern toolchains). */
    fprintf(out, "\t.section .note.GNU-stack,\"\",@progbits\n");
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