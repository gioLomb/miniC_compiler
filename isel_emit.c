/**
 * @file isel_emit.c
 * @brief AT&T x86-64 assembly printer for post-regalloc MachProgram.
 *
 * Writes into a growable buffer then a single fwrite — not fprintf per field.
 */

#include "instr_selector.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} AsmBuf;

static void ab_reserve(AsmBuf *b, size_t n) {
    if (b->len + n <= b->cap) return;
    size_t cap = b->cap ? b->cap : (1 << 20);
    size_t need = b->len + n;
    while (cap < need) cap *= 2;
    char *nb = realloc(b->buf, cap);
    if (!nb) abort();
    b->buf = nb;
    b->cap = cap;
}

static void ab_mem(AsmBuf *b, const char *s, size_t n) {
    ab_reserve(b, n);
    memcpy(b->buf + b->len, s, n);
    b->len += n;
}

static void ab_s(AsmBuf *b, const char *s) {
    ab_mem(b, s, strlen(s));
}

static void ab_c(AsmBuf *b, char c) {
    ab_reserve(b, 1);
    b->buf[b->len++] = c;
}

static void ab_i(AsmBuf *b, long v) {
    char tmp[32];
    char *p = tmp + sizeof(tmp);
    unsigned long u;
    if (v < 0) {
        ab_c(b, '-');
        u = (unsigned long)(-(v + 1)) + 1UL;
    } else {
        u = (unsigned long)v;
    }
    if (u == 0) {
        ab_c(b, '0');
        return;
    }
    while (u) {
        *--p = (char)('0' + (u % 10));
        u /= 10;
    }
    ab_mem(b, p, (size_t)(tmp + sizeof(tmp) - p));
}

static void ab_uhex(AsmBuf *b, unsigned v) {
    static const char hex[] = "0123456789abcdef";
    char tmp[8];
    int n = 0;
    if (v == 0) {
        ab_c(b, '0');
        return;
    }
    while (v && n < 8) {
        tmp[n++] = hex[v & 0xF];
        v >>= 4;
    }
    while (n--) ab_c(b, tmp[n]);
}

static void isel_emit_operand(const MachOperand *o, AsmBuf *b) {
    switch (o->kind) {
    case MO_NONE:   break;
    case MO_VREG:   ab_s(b, "%v"); ab_i(b, o->vregId); break;
    case MO_VREG_F: ab_s(b, "%fv"); ab_i(b, o->vregId); break;
    case MO_PHYS:
        ab_s(b, is_xmm_phys(o->physReg) ? phys_name_xmm[o->physReg - PHYS_XMM0]
                                        : phys_name64[o->physReg]);
        break;
    case MO_IMM:    ab_c(b, '$'); ab_i(b, o->imm); break;
    case MO_LABEL:  ab_s(b, ".L"); ab_i(b, o->labelId); break;
    case MO_FUNC:   ab_s(b, o->func); break;
    case MO_GLOBAL: ab_s(b, o->globalName); ab_s(b, "(%rip)"); break;
    case MO_STACK:  ab_c(b, '-'); ab_i(b, o->stackOff); ab_s(b, "(%rbp)"); break;
    case MO_MEM:
        if (o->mem.disp) ab_i(b, o->mem.disp);
        ab_c(b, '(');
        if (o->mem.baseVreg  >= 0) ab_s(b, phys_name64[o->mem.baseVreg]);
        if (o->mem.indexVreg >= 0) {
            ab_c(b, ',');
            ab_s(b, phys_name64[o->mem.indexVreg]);
            ab_c(b, ',');
            ab_i(b, o->mem.scale);
        }
        ab_c(b, ')');
        break;
    case MO_FIMM:
        ab_s(b, "$0x");
        ab_uhex(b, (unsigned)(int)o->fimm);
        break;
    }
}

static void isel_emit_bss_section(AsmBuf *b, const IRProgram *ir) {
    int section_emitted = 0;

    for (int i = 0; i < ir->globalCount; i++) {
        const IRGlobalVar *g = &ir->globals[i];
        if (g->initCount > 0) continue;

        if (!section_emitted) {
            ab_s(b, "\t.bss\n");
            section_emitted = 1;
        }

        int nelems = g->isArray ? g->arraySize : 1;
        ab_s(b, "\t.globl "); ab_s(b, g->name); ab_c(b, '\n');
        ab_s(b, g->name); ab_s(b, ":\n");
        ab_s(b, "\t.zero "); ab_i(b, (long)nelems * 8); ab_c(b, '\n');
    }
}

static void isel_emit_data_section(AsmBuf *b, const IRProgram *ir) {
    int section_emitted = 0;

    for (int i = 0; i < ir->globalCount; i++) {
        const IRGlobalVar *g = &ir->globals[i];
        if (g->initCount == 0) continue;

        if (!section_emitted) {
            ab_s(b, "\t.data\n");
            section_emitted = 1;
        }

        int nelems = g->isArray ? g->arraySize : 1;
        ab_s(b, "\t.globl "); ab_s(b, g->name); ab_c(b, '\n');
        ab_s(b, g->name); ab_s(b, ":\n");
        for (int j = 0; j < nelems; j++) {
            long val = (j < g->initCount) ? g->initVals[j] : 0L;
            ab_s(b, "\t.quad "); ab_i(b, val); ab_c(b, '\n');
        }
    }
}

static void isel_emit_globals(AsmBuf *b, const IRProgram *ir) {
    if (!ir || ir->globalCount == 0) return;
    isel_emit_bss_section(b, ir);
    isel_emit_data_section(b, ir);
}

static int isel_emit_fixed_encoding_instr(const MachInstr *in, int frameSize, AsmBuf *b) {
    switch (in->op) {
    case MACH_LABEL:
        ab_s(b, ".L"); ab_i(b, in->dst.labelId); ab_s(b, ":\n"); return 1;
    case MACH_FUNC_BEGIN:
        ab_s(b, "\tpushq\t%rbp\n\tmovq\t%rsp, %rbp\n");
        if (frameSize > 0) {
            ab_s(b, "\tsubq\t$"); ab_i(b, frameSize); ab_s(b, ", %rsp\n");
        }
        return 1;
    case MACH_RET:
        ab_s(b, "\tleave\n\tret\n"); return 1;
    case MACH_CQO:
        ab_s(b, "\tcqo\n"); return 1;
    case MACH_IDIV:
        ab_s(b, "\tidivq\t"); isel_emit_operand(&in->dst, b); ab_c(b, '\n'); return 1;
    case MACH_NEG:
        ab_s(b, "\tnegq\t"); isel_emit_operand(&in->dst, b); ab_c(b, '\n'); return 1;
    case MACH_NOT:
        ab_s(b, "\tnotq\t"); isel_emit_operand(&in->dst, b); ab_c(b, '\n'); return 1;
    case MACH_PUSH:
        ab_s(b, "\tpushq\t"); isel_emit_operand(&in->dst, b); ab_c(b, '\n'); return 1;
    case MACH_POP:
        ab_s(b, "\tpopq\t"); isel_emit_operand(&in->dst, b); ab_c(b, '\n'); return 1;
    case MACH_CALL:
        ab_s(b, "\tcall\t"); isel_emit_operand(&in->dst, b); ab_c(b, '\n'); return 1;
    case MACH_LEA:
        ab_s(b, "\tleaq\t");
        isel_emit_operand(&in->src1, b);
        ab_s(b, ", ");
        isel_emit_operand(&in->dst, b);
        ab_c(b, '\n'); return 1;
    case MACH_SETE:  ab_s(b, "\tsete\t%al\n");  return 1;
    case MACH_SETNE: ab_s(b, "\tsetne\t%al\n"); return 1;
    case MACH_SETL:  ab_s(b, "\tsetl\t%al\n");  return 1;
    case MACH_SETLE: ab_s(b, "\tsetle\t%al\n"); return 1;
    case MACH_SETG:  ab_s(b, "\tsetg\t%al\n");  return 1;
    case MACH_SETGE: ab_s(b, "\tsetge\t%al\n"); return 1;
    case MACH_SETB:  ab_s(b, "\tsetb\t%al\n");  return 1;
    case MACH_SETBE: ab_s(b, "\tsetbe\t%al\n"); return 1;
    case MACH_SETA:  ab_s(b, "\tseta\t%al\n");  return 1;
    case MACH_SETAE: ab_s(b, "\tsetae\t%al\n"); return 1;
    case MACH_JMP:   ab_s(b, "\tjmp\t.L");  ab_i(b, in->dst.labelId); ab_c(b, '\n'); return 1;
    case MACH_JE:    ab_s(b, "\tje\t.L");   ab_i(b, in->dst.labelId); ab_c(b, '\n'); return 1;
    case MACH_JNE:   ab_s(b, "\tjne\t.L");  ab_i(b, in->dst.labelId); ab_c(b, '\n'); return 1;
    case MACH_JL:    ab_s(b, "\tjl\t.L");   ab_i(b, in->dst.labelId); ab_c(b, '\n'); return 1;
    case MACH_JLE:   ab_s(b, "\tjle\t.L");  ab_i(b, in->dst.labelId); ab_c(b, '\n'); return 1;
    case MACH_JG:    ab_s(b, "\tjg\t.L");   ab_i(b, in->dst.labelId); ab_c(b, '\n'); return 1;
    case MACH_JGE:   ab_s(b, "\tjge\t.L");  ab_i(b, in->dst.labelId); ab_c(b, '\n'); return 1;
    case MACH_JB:    ab_s(b, "\tjb\t.L");   ab_i(b, in->dst.labelId); ab_c(b, '\n'); return 1;
    case MACH_JBE:   ab_s(b, "\tjbe\t.L");  ab_i(b, in->dst.labelId); ab_c(b, '\n'); return 1;
    case MACH_JA:    ab_s(b, "\tja\t.L");   ab_i(b, in->dst.labelId); ab_c(b, '\n'); return 1;
    case MACH_JAE:   ab_s(b, "\tjae\t.L");  ab_i(b, in->dst.labelId); ab_c(b, '\n'); return 1;
    default: return 0;
    }
}

static void isel_emit_generic_instr(const MachInstr *in, AsmBuf *b) {
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
    case MACH_MOVSS:    mnem = "movss";     break;
    case MACH_ADDSS:    mnem = "addss";     break;
    case MACH_SUBSS:    mnem = "subss";     break;
    case MACH_MULSS:    mnem = "mulss";     break;
    case MACH_DIVSS:    mnem = "divss";     break;
    case MACH_UCOMISS:  mnem = "ucomiss";   break;
    case MACH_XORPS:    mnem = "xorps";     break;
    case MACH_CVTSI2SS: mnem = "cvtsi2ssq"; break;
    case MACH_MOVQ_TO_XMM: mnem = "movq";   break;
    default:         mnem = "???";    break;
    }

    ab_c(b, '\t'); ab_s(b, mnem); ab_c(b, '\t');

    if (in->op == MACH_LOAD || in->op == MACH_STORE) {
        isel_emit_operand(&in->src1, b);
        ab_s(b, ", ");
        isel_emit_operand(&in->dst, b);
    } else if (in->src2.kind != MO_NONE ) {
        isel_emit_operand(&in->src1, b);
        ab_s(b, ", ");
        isel_emit_operand(&in->dst, b);
    } else if (in->src1.kind != MO_NONE ) {
        isel_emit_operand(&in->src1, b);
        if (in->dst.kind != MO_NONE) {
            ab_s(b, ", ");
            isel_emit_operand(&in->dst, b);
        }
    } else {
        isel_emit_operand(&in->dst, b);
    }
    ab_c(b, '\n');
}

static void isel_emit_function_body(const MachFunction *f, AsmBuf *b) {
    const int count = f->count;
    const MachInstr *instrs = f->instrs;
    const int frameSize = f->frameSize;

    for (int i = 0; i < count; i++) {
        const MachInstr *in = &instrs[i];
        if (!isel_emit_fixed_encoding_instr(in, frameSize, b))
            isel_emit_generic_instr(in, b);
    }
}

void isel_emit_asm(const MachProgram *mp, const IRProgram *ir, FILE *out) {
    AsmBuf b = {0};
    ab_reserve(&b, 1 << 20);

    isel_emit_globals(&b, ir);

    ab_s(&b, "\t.text\n");
    for (int fi = 0; fi < mp->count; fi++) {
        const MachFunction *f = mp->functions[fi];
        ab_s(&b, "\t.globl "); ab_s(&b, f->name); ab_c(&b, '\n');
        ab_s(&b, f->name); ab_s(&b, ":\n");
        isel_emit_function_body(f, &b);
        ab_c(&b, '\n');
    }
    ab_s(&b, "\t.section .note.GNU-stack,\"\",@progbits\n");

    if (b.len)
        fwrite(b.buf, 1, b.len, out);
    free(b.buf);
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
