/**
 * @file sched_utils.h
 * @brief Utility predicates and operand extractors for the instruction scheduler.
 *
 * Provides three groups of inline helpers consumed exclusively by the DAG
 * builder (sched_dag.c) and the list scheduler (sched.c):
 *
 *  1. **Latency table** (sched_latency_of)
 *     Static per-opcode cycle counts derived from Agner Fog's Haswell /
 *     Broadwell instruction tables.  Used by dag_build() to initialise each
 *     DAGNode.height and by the backward height-propagation pass to compute
 *     the latency-weighted critical-path length toward any sink node.
 *
 *  2. **Instruction predicates**
 *     - sched_is_pinned       — control-flow terminators + structural markers
 *                               that must stay at fixed positions and are never
 *                               placed in the ready heap.
 *     - sched_has_side_effect — memory writes, stack ops, calls, and integer
 *                               divide/sign-extend sequences that must be
 *                               serialised relative to each other.
 *     - sched_is_jcc          — conditional branch opcodes; used by the
 *                               macro-fusion detector in dag_build().
 *     - sched_is_cmp_or_test  — comparison opcodes that may fuse with a
 *                               following Jcc into a single micro-op.
 *
 *  3. **Operand / def-use extractors** (sched_reg, sched_reg_idx, sched_def,
 *     sched_uses)
 *     Translate MachOperand fields into flat integer register ids suitable
 *     for the SparseMap-based renaming pass inside dag_build().  Physical
 *     registers are offset by nextVreg so that vregs and physregs share a
 *     single flat id space without collision.
 *
 * All functions are static inline — zero call overhead at the hot scheduling
 * inner loop.  No heap allocation is performed here.
 *
 * Limitation: latency values are single-cycle averages; port-throughput
 * bottlenecks and cache-miss penalties are not modelled.
 */

#ifndef SCHED_UTILS_H
#define SCHED_UTILS_H
#include "instr_query.h" 
#include "instr_selector.h"


/**
 * @brief Return the execution latency in cycles for machine opcode @p op.
 *
 * Values are conservative Haswell/Broadwell estimates:
 *   - Simple ALU (ADD, SUB, NEG, NOT, XOR, SAL, MOV, CMP, TEST, SETcc,
 *     branches, CQO, LEA): 1 cycle
 *   - Multiply (IMUL): 3 cycles
 *   - Integer divide (IDIV): 20 cycles (worst-case 64-bit)
 *   - Memory (LOAD, STORE, PUSH, POP): 4 cycles
 *   - CALL / RET: 3 cycles (pipeline drain approximation)
 *   - Structural markers (LABEL, FUNC_BEGIN, FUNC_END): 0 cycles
 *
 * @param op  Machine opcode to query.
 * @return    Estimated latency in cycles (≥ 0).
 */
static inline int sched_latency_of(MachOpCode op) {
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
    case MACH_LEA:  return 1;   // address-generation unit: 1 cycle
    case MACH_LABEL: case MACH_FUNC_BEGIN: case MACH_FUNC_END: return 0;
    default: return 1;
    }
}


/**
 * @brief Return non-zero if @p op must stay at a fixed position in the block.
 *
 * Pinned instructions are never placed in the ready heap by the list
 * scheduler — they are emitted in their original program order during
 * phase 3 (flush).  Two categories are pinned:
 *   - Structural markers: LABEL, FUNC_BEGIN, FUNC_END (must be first).
 *   - Control-flow terminators: JMP, all Jcc variants, RET (must be last).
 *
 * @param op  Opcode to test.
 * @return    1 if the instruction cannot be reordered, 0 otherwise.
 */
static inline int sched_is_pinned(MachOpCode op) {
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
 * @brief Return non-zero if @p op has observable side effects beyond its dst.
 *
 * Side-effecting instructions must be emitted in their original relative
 * order; dag_build() chains them through the lastSideEffect pointer to
 * enforce this.  Covered:
 *   - Memory writes: STORE, PUSH, POP (read-modify stack pointer).
 *   - Calls: CALL (arbitrary memory and register side effects).
 *   - Division / sign-extend: IDIV, CQO (implicit RAX/RDX writes).
 *
 * @param op  Opcode to test.
 * @return    1 if serialisation with other side-effecting instructions is
 *            required, 0 otherwise.
 */
static inline int sched_has_side_effect(MachOpCode op) {
    switch (op) {
    case MACH_STORE: case MACH_PUSH: case MACH_POP:
    case MACH_CALL:  case MACH_IDIV: case MACH_CQO: return 1;
    default:                                          return 0;
    }
}

/**
 * @brief Return non-zero if @p op is a conditional branch (Jcc).
 *
 * Used by the macro-fusion detector in dag_build(): when a CMP or TEST is
 * immediately followed by a Jcc, the CMP/TEST is pinned (pinnedForFusion=1)
 * so the list scheduler preserves their adjacency and the Intel decoder can
 * fuse them into a single micro-op.
 *
 * @param op  Opcode to test.
 * @return    1 for JE, JNE, JL, JLE, JG, JGE; 0 otherwise.
 */
static inline int sched_is_jcc(MachOpCode op) {
    switch (op) {
    case MACH_JE: case MACH_JNE:
    case MACH_JL: case MACH_JLE:
    case MACH_JG: case MACH_JGE: return 1;
    default:                       return 0;
    }
}

/**
 * @brief Return non-zero if @p op is a comparison instruction (CMP or TEST).
 *
 * Paired with sched_is_jcc() to detect fuseable CMP/TEST + Jcc sequences.
 *
 * @param op  Opcode to test.
 * @return    1 for MACH_CMP and MACH_TEST; 0 otherwise.
 */
static inline int sched_is_cmp_or_test(MachOpCode op) {
    return op == MACH_CMP || op == MACH_TEST;
}



/**
 * @brief Extract the primary register id from a MachOperand.
 *
 * Dispatch rules:
 *   MO_VREG → vregId  (virtual register id, already in [0, nextVreg))
 *   MO_PHYS → nextVreg + physReg  (PHYS_AL mapped to PHYS_RAX before offset)
 *   MO_MEM  → base register id (the baseVreg field of the SIB addressing mode)
 *   other   → -1  (immediate, label, func, stack slot, none)
 *
 * The PHYS_AL → PHYS_RAX normalisation avoids treating the 8-bit %al alias
 * as a separate register in the renaming table, preventing spurious WAW edges
 * between SETcc (%al write) and RAX-using instructions.
 *
 * @param o        Operand to query.
 * @param nextVreg Base offset for physical-register ids.
 * @return         Non-negative register id, or -1 if the operand carries no id.
 */
static inline int sched_reg(const MachOperand *o, int nextVreg) {
    switch (o->kind) {
    case MO_VREG: return o->vregId;
    case MO_PHYS: return nextVreg + ((o->physReg == PHYS_AL) ? PHYS_RAX : o->physReg);
    case MO_MEM:  return (o->mem.baseVreg >= 0) ? o->mem.baseVreg : -1;
    default:      return -1;
    }
}

/**
 * @brief Extract the index register id from an MO_MEM operand.
 *
 * SIB addressing modes (base + index*scale + disp) carry a second register
 * in the indexVreg field.  dag_build() calls this alongside sched_reg() to
 * ensure RAW edges are added for both the base and index registers of a
 * memory operand.
 *
 * @param o  Operand to query.
 * @return   indexVreg if the operand is MO_MEM with a valid index, else -1.
 */
static inline int sched_reg_idx(const MachOperand *o) {
    return (o->kind == MO_MEM && o->mem.indexVreg >= 0) ? o->mem.indexVreg : -1;
}

/**
 * @brief Return the single register id defined (written) by instruction @p in.
 *
 * Instructions that do not produce a register result return -1:
 *   CMP, TEST, all Jcc/JMP, CALL, RET, PUSH, STORE, CQO, structural markers.
 * All other instructions write to their dst operand; sched_reg() is used to
 * extract the id from that operand.
 *
 * @param in       Instruction to inspect.
 * @param nextVreg Base offset for physical-register ids (forwarded to sched_reg).
 * @return         Register id of the defined operand, or -1 if none.
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
 * @brief Collect all register ids read (used) by instruction @p in.
 *
 * Fills @p out with the ids of every register the instruction reads,
 * including SIB index registers and instruction-specific implicit reads:
 *   - STORE: reads both base and index registers of the destination MO_MEM
 *            (the address is computed from them).
 *   - PUSH:  reads the dst register (source of the stack write).
 *   - IDIV:  reads the dst register (the divisor operand).
 *   - CQO:   reads the dst register (RAX, the value to sign-extend).
 *
 * @param in       Instruction to inspect.
 * @param nextVreg Base offset for physical-register ids.
 * @param out      Output array; caller must provide at least 5 entries.
 * @param n        Set to the number of ids written into @p out.
 */
/**
 * @brief Collect all register ids read (used) by instruction @p in.
 *
 * Fills @p out with the ids of every register the instruction reads,
 * including SIB index registers and instruction-specific implicit reads:
 *   - STORE: reads both base and index registers of the destination MO_MEM
 *            (the address is computed from them).
 *   - PUSH:  reads the dst register (source of the stack write).
 *   - IDIV:  reads the dst register (the divisor operand).
 *   - CQO:   reads the dst register (RAX, the value to sign-extend).
 *   - RMW:   (ADD, SUB, IMUL, SAL, NEG, NOT, XOR) dst is read before being written.
 *
 * @param in       Instruction to inspect.
 * @param nextVreg Base offset for physical-register ids.
 * @param out      Output array; caller must provide at least 5 entries.
 * @param n        Set to the number of ids written into @p out.
 */
static inline void sched_uses(const MachInstr *in, int nextVreg,
                               int out[], int *n) {
    *n = 0;
    int r;

    // helper macro: append r to out[] if valid, then check next candidate
#define SCHED_TRY(x) if ((r = (x)) >= 0) out[(*n)++] = r
    SCHED_TRY(sched_reg    (&in->src1, nextVreg));
    SCHED_TRY(sched_reg_idx(&in->src1));
    SCHED_TRY(sched_reg    (&in->src2, nextVreg));
    SCHED_TRY(sched_reg_idx(&in->src2));

    // instruction-specific additional uses not visible in src1/src2
    switch (in->op) {
    case MACH_STORE:
        // dst is a MO_MEM address: both base and index are read to form the EA
        SCHED_TRY(sched_reg    (&in->dst, nextVreg));
        SCHED_TRY(sched_reg_idx(&in->dst));
        break;
    case MACH_PUSH:
    case MACH_IDIV:
    case MACH_CQO:
        // dst field holds the source value (push) or divisor/input (idiv/cqo)
        SCHED_TRY(sched_reg(&in->dst, nextVreg));
        break;
    default:
        // FIX: Istruzioni Read-Modify-Write (ADD, SUB, IMUL, SAL, NEG, NOT, XOR)
        // leggono 'dst' prima di scriverci.
        if (instr_is_rmw(in->op)) {
            SCHED_TRY(sched_reg(&in->dst, nextVreg));
        }
        break;
    }
#undef SCHED_TRY
}

#endif /* SCHED_UTILS_H */