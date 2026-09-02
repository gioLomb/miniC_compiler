/**
 * @file instr_selector.h
 * @brief Instruction selection: lowers the linear IR to machine instructions
 *        targeting x86-64 (System V AMD64 ABI).
 */

#ifndef ISEL_H
#define ISEL_H

#include "ir.h"
#include <stdio.h>

/* =========================================================================
 * Calling-convention constants
 * ========================================================================= */

#define NUM_ARG_REGS 6
#define MAX_PARAMS   64


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
    MACH_LEA,                       /**< leaq globalname(%rip), dst          */
    MACH_LABEL, MACH_FUNC_BEGIN, MACH_FUNC_END,
} MachOpCode;

/* =========================================================================
 * Operand kind discriminant
 * ========================================================================= */

typedef enum {
    MO_NONE,
    MO_VREG,
    MO_PHYS,
    MO_IMM,
    MO_FIMM,
    MO_LABEL,
    MO_FUNC,
    MO_MEM,
    MO_STACK,
    MO_GLOBAL,  /**< Global symbol reference: emitted as name(%rip).        */
} MachOperandKind;

/* =========================================================================
 * Physical register enumeration
 * ========================================================================= */

typedef enum {
    /* caller-saved (9 registers, indices 0..8) */
    PHYS_RAX = 0,
    PHYS_RCX,
    PHYS_RDX,
    PHYS_RSI,
    PHYS_RDI,
    PHYS_R8,
    PHYS_R9,
    PHYS_R10,
    PHYS_R11,
    /* callee-saved (5 registers, indices 9..13) */
    PHYS_RBX,
    PHYS_R12,
    PHYS_R13,
    PHYS_R14,
    PHYS_R15,
    /* reserved */
    PHYS_RBP,
    PHYS_RSP,
    /* 8-bit alias of RAX; normalised by regalloc_utils before graph build */
    PHYS_AL,
    PHYS_COUNT
} MachPhysReg;

#define PHYS_ALLOCATABLE        14
#define PHYS_CALLER_SAVED_COUNT  9
#define PHYS_CALLEE_SAVED_COUNT  5

/* =========================================================================
 * MachOperand
 * ========================================================================= */

typedef struct {
    MachOperandKind kind;
    union {
        int        vregId;
        int        physReg;
        long       imm;
        float      fimm;
        int        labelId;
        const char *func;
        int        stackOff;
        struct {
            int baseVreg;
            int indexVreg;
            int scale;
            int disp;
        } mem;
        const char *globalName;  /**< MO_GLOBAL: symbol name, emitted as name(%rip). */
    };
} MachOperand;

/* =========================================================================
 * MachInstr
 * ========================================================================= */

typedef struct {
    MachOpCode     op;
    MachOperand dst, src1, src2;
    int         scale;
    int         loopDepth;
} MachInstr;

/* =========================================================================
 * MachFunction
 * ========================================================================= */

typedef struct {
    const char *name;
    MachInstr  *instrs;
    int         count;
    int         capacity;
    int         frameSize;
    int         nextVreg;
} MachFunction;

/* =========================================================================
 * MachProgram
 * ========================================================================= */

typedef struct {
    MachFunction **functions;
    int            count;
    int            capacity;
} MachProgram;

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * @brief Translate an IR program into a machine program.
 *
 * Global variable information from @p ir is passed to select_function() so
 * it can emit RIP-relative LEA/LOAD/STORE sequences for global accesses.
 */
MachProgram *isel_select(const IRProgram *ir);

/**
 * @brief Print AT&T x86-64 assembly for @p mp to @p out.
 *
 * @param mp  Machine program (post-regalloc).
 * @param ir  IR program — needed to emit .data/.bss global declarations.
 *            May be NULL, in which case no data sections are emitted.
 * @param out Output stream.
 */
void isel_emit_asm(const MachProgram *mp, const IRProgram *ir, FILE *out);

/**
 * @brief Free all memory owned by @p mp.
 */
void mach_free(MachProgram *mp);

#endif /* ISEL_H */