#ifndef ISEL_H
#define ISEL_H

#include "ir.h"
#include <stdio.h>

typedef enum {
    MACH_MOV, MACH_MOVSX,
    MACH_ADD, MACH_SUB, MACH_IMUL, MACH_IDIV,
    MACH_NEG, MACH_SAL, MACH_CQO,
    MACH_NOT, MACH_XOR,
    MACH_CMP, MACH_TEST,
    MACH_SETE, MACH_SETNE, MACH_SETL, MACH_SETLE, MACH_SETG, MACH_SETGE,
    MACH_JMP, MACH_JE, MACH_JNE, MACH_JL, MACH_JLE, MACH_JG, MACH_JGE,
    MACH_LOAD, MACH_STORE,
    MACH_PUSH, MACH_POP,
    MACH_CALL, MACH_RET,
    MACH_LABEL, MACH_FUNC_BEGIN, MACH_FUNC_END,
} MachOp;

typedef enum {
    MO_NONE,
    MO_VREG,
    MO_PHYS,
    MO_IMM,
    MO_FIMM,
    MO_LABEL,
    MO_FUNC,
    MO_MEM,
    MO_STACK,     /* slot di spill: -stackOff(%rbp), assegnato da regalloc */
} MachOperandKind;

/*
 * Registri fisici x86-64. Ordine significativo per regalloc:
 * indici 0..PHYS_CALLER_SAVED_COUNT-1 = caller-saved,
 * indici PHYS_CALLER_SAVED_COUNT..PHYS_ALLOCATABLE-1 = callee-saved.
 * RBP/RSP riservati (mai un colore). AL alias di RAX, normalizzato
 * dal regalloc prima di costruire il grafo di interferenza.
 */
typedef enum {
    /* caller-saved (9): indici 0..8 */
    PHYS_RAX = 0,
    PHYS_RCX,
    PHYS_RDX,
    PHYS_RSI,
    PHYS_RDI,
    PHYS_R8,
    PHYS_R9,
    PHYS_R10,
    PHYS_R11,
    /* callee-saved (5): indici 9..13 */
    PHYS_RBX,
    PHYS_R12,
    PHYS_R13,
    PHYS_R14,
    PHYS_R15,
    /* riservati: mai allocabili */
    PHYS_RBP,
    PHYS_RSP,
    /* alias RAX, non colore autonomo */
    PHYS_AL,
    PHYS_COUNT
} MachPhysReg;

#define PHYS_ALLOCATABLE        14
#define PHYS_CALLER_SAVED_COUNT  9
#define PHYS_CALLEE_SAVED_COUNT  5

typedef struct {
    MachOperandKind kind;
    union {
        int        vregId;
        int        physReg;
        long       imm;
        float      fimm;
        int        labelId;
        const char *func;
        int        stackOff;   /* MO_STACK: offset positivo, emesso -N(%rbp) */
        struct {
            int baseVreg;    /* indice vreg (pre-regalloc) o MachPhysReg (post) */
            int indexVreg;
            int scale;
            int disp;
        } mem;
    };
} MachOperand;

typedef struct {
    MachOp      op;
    MachOperand dst, src1, src2;
    int         scale;
    int         loopDepth;   /* ereditato dall'IRInstr; usato da regalloc per
                                 pesare il costo di spill (10^loopDepth) */
} MachInstr;

typedef struct {
    const char *name;
    MachInstr  *instrs;
    int         count, capacity;
    int         frameSize;
    int         nextVreg;
} MachFunction;

typedef struct {
    MachFunction **functions;
    int            count, capacity;
} MachProgram;

MachProgram *isel_select(const IRProgram *ir);
void         isel_emit_asm(const MachProgram *mp, FILE *out);
void         mach_free(MachProgram *mp);

#endif