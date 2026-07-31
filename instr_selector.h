#ifndef ISEL_H
#define ISEL_H

#include "ir.h"
#include <stdio.h>
/* =========================================================================
 * Instruction Selector: IRFunction → MachFunction (virtual regs, x86-64)
 *
 * Strategia: macro-expansion diretta TAC → x86-64 + peephole locale.
 * Ogni IR op viene espansa in 1-4 istruzioni macchina usando virtual
 * registers (ID interi); il register allocator (futuro) mapperà vreg →
 * registri fisici / stack slot.
 *
 * Peephole integrati durante la selezione:
 *   1. CMP+Jcc fusion: if_false su risultato di comparazione → cmp + jcc
 *      diretta, senza materializzare il booleano in un vreg.
 *   2. MUL power-of-2: t = a * K con K = 2^n → sal (shift left).
 *      Complementare a SR che copre solo variabili induttive in loop.
 * ========================================================================= */

/* ---- Opcodes macchina -------------------------------------------------- */
typedef enum {
    /* Data movement */
    MACH_MOV,          /* dst = src                                          */
    MACH_MOVSX,        /* dst = sign-extend(src) — per setcc → 64bit        */

    /* Arithmetic */
    MACH_ADD,          /* dst += src                                         */
    MACH_SUB,          /* dst -= src                                         */
    MACH_IMUL,         /* dst *= src  (2-operand form)                       */
    MACH_IDIV,         /* rax/rdx ÷ src; quoz → rax, rem → rdx             */
    MACH_NEG,          /* dst = -dst                                         */
    MACH_SAL,          /* dst <<= imm  (shift arithmetic left)               */
    MACH_CQO,          /* sign-extend rax → rdx:rax  (prima di idiv)        */

    /* Logic/Bitwise */
    MACH_NOT,          /* dst = ~dst  (bitwise NOT; usato per logico !)      */
    MACH_XOR,          /* dst ^= src                                         */

    /* Comparison & conditional set */
    MACH_CMP,          /* FLAGS ← src1 - src2 (non scrive dst)              */
    MACH_TEST,         /* FLAGS ← src1 & src2 (non scrive dst)              */
    MACH_SETE,         /* al = (ZF)                                          */
    MACH_SETNE,        /* al = (!ZF)                                         */
    MACH_SETL,         /* al = (SF≠OF)                                       */
    MACH_SETLE,        /* al = (ZF | SF≠OF)                                  */
    MACH_SETG,         /* al = (!ZF & SF=OF)                                 */
    MACH_SETGE,        /* al = (SF=OF)                                       */

    /* Control flow */
    MACH_JMP,          /* unconditional jump                                 */
    MACH_JE,           /* jump if equal (ZF)                                 */
    MACH_JNE,          /* jump if not equal (!ZF)                            */
    MACH_JL,           /* jump if less                                       */
    MACH_JLE,          /* jump if less or equal                              */
    MACH_JG,           /* jump if greater                                    */
    MACH_JGE,          /* jump if greater or equal                           */

    /* Memory */
    MACH_LOAD,         /* dst = [base + index*scale]                         */
    MACH_STORE,        /* [base + index*scale] = src                         */

    /* Call / return */
    MACH_PUSH,         /* push src onto stack                                */
    MACH_POP,          /* pop from stack into dst                            */
    MACH_CALL,         /* call function                                       */
    MACH_RET,          /* return                                             */

    /* Pseudo-instructions (non emesse come testo, usate internamente) */
    MACH_LABEL,        /* definizione di etichetta                           */
    MACH_FUNC_BEGIN,   /* prologo funzione (push rbp; mov rbp,rsp; sub rsp,N)*/
    MACH_FUNC_END,     /* epilogo (leave; ret) — non emessa, solo marcatore */
} MachOp;

/* ---- Operandi macchina ------------------------------------------------- */
typedef enum {
    MO_NONE,
    MO_VREG,      /* virtual register (ID intero)                           */
    MO_PHYS,      /* registro fisico (indice in MachPhysReg)                */
    MO_IMM,       /* costante intera immediata                              */
    MO_FIMM,      /* costante float immediata (per .data label future)      */
    MO_LABEL,     /* etichetta IR (labelId)                                 */
    MO_FUNC,      /* nome funzione (per CALL)                               */
    MO_MEM,       /* [base_vreg + index_vreg * scale + disp]               */
} MachOperandKind;

/* Registri fisici usati nell'espansione (scratch temporanei per singola op) */
typedef enum {
    PHYS_RAX = 0,
    PHYS_RCX,
    PHYS_RDX,
    PHYS_RBP,
    PHYS_RSP,
    PHYS_RDI,
    PHYS_RSI,
    PHYS_R8,
    PHYS_R9,
    PHYS_AL,       /* byte register per setcc */
    PHYS_COUNT
} MachPhysReg;

typedef struct {
    MachOperandKind kind;
    union {
        int    vregId;     /* MO_VREG                                        */
        int    physReg;    /* MO_PHYS  (valore MachPhysReg)                  */
        long   imm;        /* MO_IMM                                         */
        float  fimm;       /* MO_FIMM                                        */
        int    labelId;    /* MO_LABEL                                       */
        const char *func;  /* MO_FUNC                                        */
        struct {
            int baseVreg;  /* MO_MEM: base register (vreg, -1 se assente)   */
            int indexVreg; /* indice (vreg, -1 se assente)                   */
            int scale;     /* 1/2/4/8                                        */
            int disp;      /* spiazzamento costante                          */
        } mem;
    };
} MachOperand;

/* ---- Singola istruzione macchina --------------------------------------- */
typedef struct {
    MachOp      op;
    MachOperand dst;    /* scrittura (NONE se istr non scrive)              */
    MachOperand src1;   /* primo sorgente                                   */
    MachOperand src2;   /* secondo sorgente (NONE se unario/zero-operand)  */
    int         scale;  /* per MEM: scala dell'indice (default 8 per int64)*/
} MachInstr;

/* ---- Funzione macchina ------------------------------------------------- */
typedef struct {
    const char *name;
    MachInstr  *instrs;
    int         count;
    int         capacity;
    int         frameSize;   /* byte riservati sullo stack (16-aligned)     */
    int         nextVreg;    /* prossimo ID vreg disponibile                */
} MachFunction;

/* ---- Programma macchina ------------------------------------------------ */
typedef struct {
    MachFunction **functions;
    int            count;
    int            capacity;
} MachProgram;

/* =========================================================================
 * API pubblica
 * ========================================================================= */

/*
 * Traduce un IRProgram in MachProgram.
 * Alloca tutte le strutture con malloc; liberare con mach_free().
 */
MachProgram *isel_select(const IRProgram *ir);

/*
 * Emette il MachProgram come testo assembly x86-64 AT&T syntax su 'out'.
 * Produce un file .s linkabile con gcc/ld.
 */
void isel_emit_asm(const MachProgram *mp, FILE *out);

/*
 * Libera tutte le strutture allocate da isel_select().
 */
void mach_free(MachProgram *mp);

#endif /* ISEL_H */