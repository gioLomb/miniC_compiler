/**
 * @file sched_utils.h
 * @brief Utility predicates and operand extractors for the instruction scheduler.
 *
 * This header provides three categories of inline helpers consumed exclusively
 * by `sched_dag.c` and `sched.c`:
 *
 *  1. **Latency table** (`sched_latency_of`) — static cycle estimates for
 *     each MachOp, sourced from Agner Fog's Haswell/Broadwell instruction
 *     tables.  Used by `build_dag()` to weight critical-path heights so that
 *     the list scheduler prioritises high-latency instructions first.
 *
 *  2. **Instruction predicates** — classify opcodes for scheduling purposes:
 *     - `sched_is_pinned`         : instruction whose position in the block is
 *                                   fixed (labels, jumps, ret — never reordered).
 *     - `sched_has_side_effect`   : instruction serialised with other side-
 *                                   effecting instructions (stores, calls, …).
 *     - `sched_is_jcc`            : conditional branch (Jcc) opcode.
 *     - `sched_is_cmp_or_test`    : CMP or TEST — paired with Jcc for fusion.
 *
 *  3. **Operand / def-use extractors** — `sched_def`, `sched_uses`, `sched_reg`,
 *     `sched_reg_idx` — used by `build_dag()` to build RAW dependency edges
 *     and perform register renaming to eliminate false WAR/WAW dependencies.
 *
 * All functions are `static inline`; no translation unit is required.
 *
 * @note
 * The latency model is intentionally coarse: it ignores cache misses, branch
 * mispredictions, and execution-port throughput.  For correctness only the
 * dependency edges matter; latencies affect scheduling quality, not correctness.
 */

#ifndef SCHED_UTILS_H
#define SCHED_UTILS_H

#include "instr_selector.h"

/* =========================================================================
 * Latency table — Agner Fog Haswell/Broadwell estimates
 * =========================================================================
 * Each entry is the worst-case output latency in cycles for the opcode.
 * LOAD/STORE assume L1-cache hits; IDIV uses the 64-bit worst case.
 * Control-flow and pseudo-opcodes have latency 0 (they are pinned and
 * therefore never compete for scheduling slots).
 * ========================================================================= */

/**
 * @brief Return the estimated output latency (cycles) for @p op.
 *
 * Used by `build_dag()` to initialise `DAGNode.latency` and compute
 * backward critical-path heights.  Higher latency → higher scheduling
 * priority (hoisted earlier by the list scheduler).
 *
 * @param op  Machine opcode.
 * @return    Latency in cycles (static estimate, never zero for real ops).
 */
static inline int sched_latency_of(MachOp op) {
    switch (op) {
    case MACH_ADD: case MACH_SUB: case MACH_NEG: case MACH_NOT:
    case MACH_XOR:  return 1;
    case MACH_IMUL: return 3;
    case MACH_IDIV: return 20;
    case MACH_SAL:  return 1;
    case MACH_MOV: case MACH_MOVSX: return 1;
    case MACH_LOAD: case MACH_STORE:
    case MACH_PUSH: case MACH_POP:  return 4;
    case MACH_CMP: case MACH_TEST:  return 1;
    case MACH_SETE: case MACH_SETNE:
    case MACH_SETL: case MACH_SETLE:
    case MACH_SETG: case MACH_SETGE: return 1;
    case MACH_JMP:
    case MACH_JE:  case MACH_JNE:
    case MACH_JL:  case MACH_JLE:
    case MACH_JG:  case MACH_JGE:  return 1;
    case MACH_CALL: case MACH_RET: return 3;
    case MACH_CQO:  return 1;
    case MACH_LABEL: case MACH_FUNC_BEGIN: case MACH_FUNC_END: return 0;
    default: return 1;
    }
}

/* =========================================================================
 * Instruction predicates
 * ========================================================================= */

/**
 * @brief Return non-zero if @p op must stay in its original position.
 *
 * Pinned instructions are emitted in fixed program order: labels, function
 * prologues/epilogues, unconditional/conditional jumps, and returns.
 * `schedule_block()` places them into the output stream before (labels,
 * prologues) or after (jumps, rets) the list-scheduled body.
 *
 * @param op  Machine opcode to test.
 * @return    1 if pinned, 0 if freely schedulable.
 */
static inline int sched_is_pinned(MachOp op) {
    switch (op) {
    case MACH_LABEL: case MACH_FUNC_BEGIN: case MACH_FUNC_END:
    case MACH_JMP:
    case MACH_JE:  case MACH_JNE:
    case MACH_JL:  case MACH_JLE:
    case MACH_JG:  case MACH_JGE:
    case MACH_RET: return 1;
    default:       return 0;
    }
}

/**
 * @brief Return non-zero if @p op has a memory or ABI side effect.
 *
 * Side-effecting instructions are serialised with each other in program
 * order via explicit DAG edges in `build_dag()`.  This prevents, e.g.,
 * a STORE from being hoisted past a CALL that might read the same memory.
 *
 * @param op  Machine opcode to test.
 * @return    1 if the instruction has a side effect, 0 otherwise.
 */
static inline int sched_has_side_effect(MachOp op) {
    switch (op) {
    case MACH_STORE: case MACH_PUSH: case MACH_POP:
    case MACH_CALL:  case MACH_IDIV: case MACH_CQO: return 1;
    default:                                          return 0;
    }
}

/**
 * @brief Return non-zero if @p op is a conditional branch (Jcc).
 *
 * Used by `build_dag()` to detect CMP/TEST + Jcc pairs eligible for
 * Intel decoder macro-fusion (`pinnedForFusion` flag).
 *
 * @param op  Machine opcode to test.
 * @return    1 for JE, JNE, JL, JLE, JG, JGE; 0 otherwise.
 */
static inline int sched_is_jcc(MachOp op) {
    switch (op) {
    case MACH_JE: case MACH_JNE:
    case MACH_JL: case MACH_JLE:
    case MACH_JG: case MACH_JGE: return 1;
    default:                       return 0;
    }
}

/**
 * @brief Return non-zero if @p op is CMP or TEST.
 *
 * Together with `sched_is_jcc`, identifies candidate macro-fusion pairs:
 * a CMP or TEST immediately followed by a Jcc is marked `pinnedForFusion`
 * and kept adjacent in the scheduled output.
 *
 * @param op  Machine opcode to test.
 * @return    1 for MACH_CMP or MACH_TEST; 0 otherwise.
 */
static inline int sched_is_cmp_or_test(MachOp op) {
    return op == MACH_CMP || op == MACH_TEST;
}

/* =========================================================================
 * Operand / def-use extractors
 * =========================================================================
 * These helpers translate MachOperand kinds into raw integer register ids
 * (vreg or physical) for dependency tracking inside the DAG builder.
 *
 * Convention:
 *   - vregs are in [0, nextVreg).
 *   - physical registers are represented as nextVreg + MachPhysReg index.
 *   - -1 means "no register" (immediate, label, absent operand).
 * ========================================================================= */

/**
 * @brief Extract the primary register id from a MachOperand.
 *
 * Dispatch:
 *   MO_VREG → vregId
 *   MO_PHYS → nextVreg + physReg  (PHYS_AL normalised to PHYS_RAX)
 *   MO_MEM  → baseVreg (the base register of the SIB addressing mode)
 *   other   → -1
 *
 * @param o        Operand to inspect.
 * @param nextVreg Base offset for physical register ids.
 * @return         Register id, or -1 if the operand carries no register.
 */
static inline int sched_reg(const MachOperand *o, int nextVreg) {
    switch (o->kind) {
    case MO_VREG: return o->vregId;
    case MO_PHYS: return nextVreg + ((o->physReg == PHYS_AL) ? PHYS_RAX : o->physReg);
    case MO_MEM:  return (o->mem.baseVreg  >= 0) ? o->mem.baseVreg  : -1;
    default:      return -1;
    }
}

/**
 * @brief Extract the *index* register id from an MO_MEM operand.
 *
 * Returns `indexVreg` when the operand is a scaled-index memory reference
 * (`disp(base, index, scale)`); returns -1 for all other operand kinds
 * or when `indexVreg` is absent (-1).
 *
 * @param o  Operand to inspect (only MO_MEM is meaningful).
 * @return   Index register vreg id, or -1.
 */
static inline int sched_reg_idx(const MachOperand *o) {
    return (o->kind == MO_MEM && o->mem.indexVreg >= 0) ? o->mem.indexVreg : -1;
}

/**
 * @brief Return the single register *defined* by instruction @p in, or -1.
 *
 * Instructions that do not write a register (CMP, TEST, jumps, CALL, RET,
 * PUSH, STORE, CQO, labels, pseudo-ops) return -1.  All others return the
 * primary register of their `dst` operand via `sched_reg`.
 *
 * This is used by `build_dag()` to apply register renaming: each new
 * definition of a register id creates a fresh "renamed" id, breaking WAR
 * and WAW anti-dependencies so that the scheduler can reorder freely.
 *
 * @param in       Instruction to inspect.
 * @param nextVreg Base offset for physical register ids.
 * @return         Defined register id, or -1.
 */
static inline int sched_def(const MachInstr *in, int nextVreg) {
    switch (in->op) {
    case MACH_CMP:  case MACH_TEST:
    case MACH_JMP:
    case MACH_JE:   case MACH_JNE:
    case MACH_JL:   case MACH_JLE:
    case MACH_JG:   case MACH_JGE:
    case MACH_CALL: case MACH_RET:
    case MACH_PUSH: case MACH_STORE:
    case MACH_CQO:
    case MACH_LABEL: case MACH_FUNC_BEGIN: case MACH_FUNC_END:
        return -1;
    default:
        return sched_reg(&in->dst, nextVreg);
    }
}

/**
 * @brief Collect all registers *read* by instruction @p in into @p out[].
 *
 * Fills @p out with up to 5 register ids and sets @p *n to the count.
 * The ids include:
 *   - Primary register of src1 and src2 (via `sched_reg`).
 *   - Index register of src1 and src2 (via `sched_reg_idx`), when present.
 *   - For STORE, PUSH, IDIV, CQO: primary register of `dst` as well
 *     (these opcodes read their `dst` operand rather than writing it, or
 *     implicitly read it before a read-modify-write).
 *   - **[BUG FIX]** For STORE: index register of `dst.mem` (the array
 *     index vreg used as the memory address index), which was previously
 *     omitted, causing the DAG builder to miss RAW edges of the form
 *     "compute index → STORE array[index]".
 *
 * @param in       Instruction to inspect.
 * @param nextVreg Base offset for physical register ids.
 * @param out      Output array; caller must provide space for at least 5 ints.
 * @param n        Set to the number of valid entries written into @p out.
 */
static inline void sched_uses(const MachInstr *in, int nextVreg,
                               int out[], int *n) {
    *n = 0;
    int r;
#define SCHED_TRY(x) if ((r = (x)) >= 0) out[(*n)++] = r
    SCHED_TRY(sched_reg    (&in->src1, nextVreg));
    SCHED_TRY(sched_reg_idx(&in->src1));
    SCHED_TRY(sched_reg    (&in->src2, nextVreg));
    SCHED_TRY(sched_reg_idx(&in->src2));
    /* STORE, PUSH, IDIV, CQO also read the dst operand. */
    switch (in->op) {
    case MACH_STORE:
        /* base register of the destination MO_MEM */
        SCHED_TRY(sched_reg    (&in->dst, nextVreg));
        SCHED_TRY(sched_reg_idx(&in->dst));
        break;
    case MACH_PUSH:
    case MACH_IDIV:
    case MACH_CQO:
        SCHED_TRY(sched_reg(&in->dst, nextVreg));
        break;
    default: break;
    }
#undef SCHED_TRY
}

#endif /* SCHED_UTILS_H */