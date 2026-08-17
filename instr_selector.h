/**
 * @file instr_selector.h
 * @brief Instruction selection: lowers the linear IR to machine instructions
 *        targeting x86-64 (System V AMD64 ABI).
 *
 * This module sits between the IR optimisation pipeline and the register
 * allocator.  It consumes an IRProgram and produces a MachProgram — a
 * parallel representation where every IRInstr has been expanded into one or
 * more MachInstr nodes that closely match real x86-64 encodings.
 *
 * Virtual registers
 * -----------------
 * At this stage physical registers are not yet assigned.  Every IR variable
 * and temporary is mapped to a *virtual register* (vreg) — a small integer
 * in [0, nextVreg).  The mapping is performed by VarMap (varmap.h) during
 * a pre-scan of the function's instruction stream so that all vregs are
 * known before any machine instruction is emitted.  isel-internal temporaries
 * (constants loaded into registers, intermediate values) are allocated above
 * the VarMap range with mfunc_new_vreg() to avoid collisions.
 *
 * The register allocator (regalloc.h) later replaces every MO_VREG with
 * either MO_PHYS (a physical register) or MO_STACK (a spill slot).
 *
 * Operand kinds (MachOperandKind)
 * --------------------------------
 *   MO_NONE   — absent operand (unused slot in a MachInstr)
 *   MO_VREG   — virtual register, identified by vregId
 *   MO_PHYS   — physical register, identified by physReg (MachPhysReg enum)
 *   MO_IMM    — 64-bit integer immediate (long)
 *   MO_FIMM   — 32-bit float immediate stored as bit pattern (int)
 *   MO_LABEL  — branch target, identified by labelId
 *   MO_FUNC   — callee name for CALL instructions
 *   MO_MEM    — memory reference: disp(baseVreg, indexVreg, scale)
 *   MO_STACK  — spill slot: -stackOff(%rbp), assigned by the register allocator
 *
 * Physical register layout (MachPhysReg)
 * ----------------------------------------
 * Registers are ordered so that the allocator can use simple index ranges:
 *
 *   indices 0..8   (PHYS_CALLER_SAVED_COUNT = 9)  caller-saved:
 *                  RAX, RCX, RDX, RSI, RDI, R8, R9, R10, R11
 *   indices 9..13  (PHYS_CALLEE_SAVED_COUNT = 5)  callee-saved:
 *                  RBX, R12, R13, R14, R15
 *   PHYS_RBP, PHYS_RSP  — reserved, never allocated
 *   PHYS_AL             — alias of RAX (8-bit); normalised to PHYS_RAX
 *                          by the register allocator before graph construction
 *
 *   PHYS_ALLOCATABLE = 14  (caller-saved + callee-saved, excluding RBP/RSP/AL)
 *
 * Calling convention (System V AMD64)
 * -------------------------------------
 * Integer arguments are passed in RDI, RSI, RDX, RCX, R8, R9 (first six).
 * Additional arguments are pushed on the stack in reverse order and popped
 * by the caller after the call.  The return value is in RAX.
 * isel_select() emits IR_PARAM as MOV into the appropriate arg register or
 * PUSH for excess arguments, followed by a CALL instruction.
 *
 * Peephole optimisations applied during selection
 * -------------------------------------------------
 *   - IR_MUL by a power of two → SAL (shift left) instead of IMUL.
 *   - IR_ADD / IR_SUB where one source equals the destination → in-place
 *     update (omits the leading MOV).
 *   - Comparison + IF_FALSE fusion: when IR_LT/LE/GT/GE/EQ/NE is
 *     immediately followed by IR_IF_FALSE on its result, the pair is
 *     lowered to CMP + Jcc (macro-fusion friendly on Intel) instead of
 *     CMP + SETcc + MOVSX + TEST + JE.
 *
 * Pipeline position
 * -----------------
 *   IR optimisation pipeline → isel_select() → sched_schedule() → regalloc()
 *
 * Public API
 * ----------
 *   isel_select()   — translate IRProgram → MachProgram
 *   isel_emit_asm() — print AT&T x86-64 assembly to a FILE
 *   mach_free()     — release all memory owned by a MachProgram
 */

#ifndef ISEL_H
#define ISEL_H

#include "ir.h"
#include <stdio.h>

/* =========================================================================
 * Calling-convention constants
 * ========================================================================= */

/** Number of integer argument registers in the System V AMD64 ABI. */
#define NUM_ARG_REGS 6

/** Maximum number of IR_PARAM instructions tracked per call site. */
#define MAX_PARAMS   64

/* =========================================================================
 * Machine opcodes
 * =========================================================================
 * Each MachOp corresponds closely to a single x86-64 instruction mnemonic.
 * Control-flow opcodes (JMP, Jcc, RET, LABEL) and pseudo-opcodes
 * (FUNC_BEGIN, FUNC_END) are included so that the instruction stream is
 * self-contained and can be printed or scheduled without external metadata.
 * ========================================================================= */

/**
 * @brief Opcodes for machine-level instructions.
 *
 * Grouped by category:
 *   - Data movement : MOV, MOVSX
 *   - Arithmetic    : ADD, SUB, IMUL, IDIV, NEG, SAL, CQO
 *   - Bitwise       : NOT, XOR
 *   - Comparison    : CMP, TEST
 *   - Conditional   : SETE, SETNE, SETL, SETLE, SETG, SETGE
 *   - Jumps         : JMP, JE, JNE, JL, JLE, JG, JGE
 *   - Memory        : LOAD, STORE
 *   - Stack         : PUSH, POP
 *   - Call/return   : CALL, RET
 *   - Pseudo        : LABEL, FUNC_BEGIN, FUNC_END
 */
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

/* =========================================================================
 * Operand kind discriminant
 * ========================================================================= */

/**
 * @brief Discriminant tag for the MachOperand union.
 *
 * Determines which field of the MachOperand union is active and how the
 * operand should be encoded in the final assembly output.
 */
typedef enum {
    MO_NONE,    /**< Absent / unused operand slot.                           */
    MO_VREG,    /**< Virtual register; active field: vregId.                 */
    MO_PHYS,    /**< Physical register; active field: physReg.               */
    MO_IMM,     /**< 64-bit integer immediate; active field: imm.            */
    MO_FIMM,    /**< 32-bit float as bit pattern; active field: fimm.        */
    MO_LABEL,   /**< Branch target label; active field: labelId.             */
    MO_FUNC,    /**< Callee name for CALL; active field: func.               */
    MO_MEM,     /**< Memory reference disp(base,index,scale); field: mem.    */
    MO_STACK,   /**< Spill slot -stackOff(%rbp); active field: stackOff.
                  *   Assigned by regalloc; not produced by isel_select().   */
} MachOperandKind;

/* =========================================================================
 * Physical register enumeration
 * =========================================================================
 * The ordering is significant: the register allocator uses index ranges to
 * distinguish caller-saved from callee-saved registers without per-register
 * if-chains.  RBP and RSP are never allocatable; AL is an alias of RAX and
 * is normalised before interference-graph construction.
 * ========================================================================= */

/**
 * @brief Physical x86-64 registers, ordered for the register allocator.
 *
 * Caller-saved (indices 0..PHYS_CALLER_SAVED_COUNT-1):
 *   RAX, RCX, RDX, RSI, RDI, R8, R9, R10, R11
 * Callee-saved (indices PHYS_CALLER_SAVED_COUNT..PHYS_ALLOCATABLE-1):
 *   RBX, R12, R13, R14, R15
 * Reserved (never allocated):
 *   RBP, RSP
 * Alias (normalised to RAX before regalloc):
 *   AL
 */
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
    /* reserved — never used as allocation colours */
    PHYS_RBP,
    PHYS_RSP,
    /* 8-bit alias of RAX; normalised by regalloc_utils before graph build */
    PHYS_AL,
    PHYS_COUNT   /**< Total number of enum values (not a valid register). */
} MachPhysReg;

/** Number of allocatable registers (caller-saved + callee-saved). */
#define PHYS_ALLOCATABLE        14
/** Number of caller-saved allocatable registers. */
#define PHYS_CALLER_SAVED_COUNT  9
/** Number of callee-saved allocatable registers. */
#define PHYS_CALLEE_SAVED_COUNT  5

/* =========================================================================
 * MachOperand — discriminated union for a single instruction operand
 * ========================================================================= */

/**
 * @brief A single operand of a machine instruction.
 *
 * The @c kind field selects which union member is active:
 *
 *   MO_VREG   → vregId   : virtual register index (pre-regalloc)
 *   MO_PHYS   → physReg  : MachPhysReg value (pre- and post-regalloc)
 *   MO_IMM    → imm      : 64-bit signed immediate
 *   MO_FIMM   → fimm     : 32-bit float stored as its IEEE 754 bit pattern
 *   MO_LABEL  → labelId  : branch target (same id space as IR labels)
 *   MO_FUNC   → func     : callee name string (not freed; owned by the IR)
 *   MO_STACK  → stackOff : positive byte offset from RBP; emitted as -N(%rbp)
 *   MO_MEM    → mem      : base/index/scale/disp addressing mode
 *   MO_NONE   → (no active field)
 */
typedef struct {
    MachOperandKind kind;
    union {
        int        vregId;   /**< MO_VREG: virtual register index.           */
        int        physReg;  /**< MO_PHYS: MachPhysReg cast to int.          */
        long       imm;      /**< MO_IMM:  64-bit immediate value.           */
        float      fimm;     /**< MO_FIMM: float immediate (bit pattern).    */
        int        labelId;  /**< MO_LABEL: branch target label id.          */
        const char *func;    /**< MO_FUNC:  callee function name.            */
        int        stackOff; /**< MO_STACK: positive offset; emitted -N(%rbp).*/
        struct {
            int baseVreg;    /**< Base register (vreg pre-regalloc, phys after). */
            int indexVreg;   /**< Index register (-1 if absent).             */
            int scale;       /**< Scale factor (1, 2, 4, or 8).             */
            int disp;        /**< Signed displacement in bytes.              */
        } mem;               /**< MO_MEM: SIB + displacement addressing.     */
    };
} MachOperand;

/* =========================================================================
 * MachInstr — a single machine instruction
 * ========================================================================= */

/**
 * @brief A single x86-64 machine instruction with up to three operands.
 *
 * The operand layout follows AT&T conventions used in isel_emit_asm():
 *   - Most instructions: dst is the destination, src1/src2 are sources.
 *   - STORE / LOAD: src1 is the memory operand, dst is the register.
 *   - CMP / TEST: src1 and src2 are both sources, dst is unused.
 *   - PUSH / POP / NEG / NOT / IDIV: dst is the single operand.
 *   - CALL: dst is MO_FUNC (callee name).
 *   - LABEL / FUNC_BEGIN / FUNC_END: dst carries the label id or is unused.
 *
 * @c loopDepth is inherited from the originating IRInstr and used by the
 * register allocator to weight spill costs (10^loopDepth), so that
 * variables live inside hot loops are preferred for register allocation.
 */
typedef struct {
    MachOp      op;
    MachOperand dst, src1, src2;
    int         scale;      /**< Operand size in bytes (default 8 for 64-bit). */
    int         loopDepth;  /**< Static loop-nesting depth; drives spill cost.  */
} MachInstr;

/* =========================================================================
 * MachFunction — machine-level function
 * ========================================================================= */

/**
 * @brief Machine-level representation of a single function.
 *
 * @c instrs is a flat, growable array of MachInstr in emission order.
 * @c nextVreg is the next available virtual register id; it starts at the
 * number of IR operands (from the VarMap pre-scan) and grows as isel emits
 * internal temporaries.  The register allocator uses this value to determine
 * the total number of vregs to colour.
 * @c frameSize is the total stack frame size in bytes, computed initially
 * from nextVreg * 8 and rounded up to a 16-byte boundary; the register
 * allocator updates it to reflect spill slots.
 */
typedef struct {
    const char *name;       /**< Function name (owned by the IR).            */
    MachInstr  *instrs;     /**< Instruction array (heap, grown via realloc).*/
    int         count;      /**< Number of valid instructions.               */
    int         capacity;   /**< Allocated capacity of instrs[].             */
    int         frameSize;  /**< Stack frame size in bytes (16-byte aligned).*/
    int         nextVreg;   /**< Next free virtual register id.              */
} MachFunction;

/* =========================================================================
 * MachProgram — machine-level translation unit
 * ========================================================================= */

/**
 * @brief Machine-level representation of an entire translation unit.
 *
 * Parallel to IRProgram: one MachFunction per IRFunction, in the same order.
 */
typedef struct {
    MachFunction **functions; /**< Heap array of function pointers.          */
    int            count;     /**< Number of valid functions.                */
    int            capacity;  /**< Allocated capacity of functions[].        */
} MachProgram;

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * @brief Translate an IR program into a machine program.
 *
 * For each IRFunction in @p ir, performs a single-pass instruction selection:
 *   1. Pre-scan all IR operands to assign vreg ids via VarMap; this ensures
 *      that every variable and temporary has a stable vreg before any
 *      machine instruction is emitted (isel-internal temporaries are
 *      allocated above this range with mfunc_new_vreg).
 *   2. Emit MACH_FUNC_BEGIN to mark the function prologue.
 *   3. Lower each IRInstr to one or more MachInstr, applying peephole
 *      optimisations (power-of-two multiply → shift, in-place ADD/SUB,
 *      comparison+branch fusion into CMP+Jcc).
 *   4. Compute an initial frameSize = nextVreg * 8, rounded to 16 bytes.
 *
 * The returned MachProgram must be released with mach_free() when no
 * longer needed.
 *
 * @param ir  IR program produced by ir_generate().
 * @return    Heap-allocated MachProgram; caller must call mach_free().
 */
MachProgram *isel_select(const IRProgram *ir);

/**
 * @brief Print AT&T x86-64 assembly for @p mp to @p out.
 *
 * Emits a .text section with one .globl label per function.  Virtual
 * registers are printed as %vN (useful for debugging pre-regalloc output).
 * Physical registers use their canonical 64-bit names (%rax, %rcx, …).
 * Spill slots are printed as -N(%rbp).
 *
 * FUNC_BEGIN is expanded to the standard prologue (push %rbp; mov %rsp,%rbp;
 * sub $N,%rsp).  RET is expanded to leave; ret.
 *
 * @param mp   MachProgram to emit (may be pre- or post-regalloc).
 * @param out  Output stream (typically stdout or an open .s file).
 */
void isel_emit_asm(const MachProgram *mp, FILE *out);

/**
 * @brief Free all memory owned by @p mp.
 *
 * Frees every MachFunction's instruction array, the function pointer array,
 * and the MachProgram struct itself.  The function name strings are owned
 * by the IR and are not freed here.
 *
 * @param mp  MachProgram to destroy; may be NULL (no-op).
 */
void mach_free(MachProgram *mp);

#endif /* ISEL_H */