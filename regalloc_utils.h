/**
 * @file regalloc_utils.h
 * @brief Instruction-analysis utilities shared across the register allocator.
 *
 * Provides three groups of functions used by regalloc.c, interference.c,
 * ra_spill.c, and sched_utils.h:
 *
 *  1. Spill-cost weighting
 *     regalloc_spill_weight() maps a static loop-nesting depth to an integer
 *     weight (10^depth), so that variables live inside hot loops are preferred
 *     for register assignment over variables that live only in cold code.
 *
 *  2. Operand extraction — explicit and implicit
 *     x86-64 instructions have operands beyond what is visible in the
 *     MachOperand fields: IDIV implicitly reads and writes RAX/RDX; CALL
 *     clobbers all caller-saved registers.  The extraction functions make
 *     these implicit data-flow edges explicit so that liveness analysis and
 *     the interference graph see the complete use-def information.
 *
 *     instr_uses()          — explicit source operands (registers read)
 *     instr_implicit_uses() — implicit reads (e.g. RAX/RDX before IDIV)
 *     instr_defs()          — explicit destination operand (register written)
 *     instr_implicit_defs() — implicit writes (e.g. caller-saved after CALL)
 *
 *  3. Instruction predicates
 *     regalloc_is_rmw()          — read-modify-write: destination is also a source
 *     regalloc_is_setcc()        — SETcc family: result goes into %al
 *     regalloc_is_ctrl_transfer() — control-flow: cannot be moved or deleted
 *
 * Physical-register indexing convention
 * --------------------------------------
 * Physical registers are represented as integers in the range
 * [nextVreg, nextVreg + PHYS_ALLOCATABLE).  The offset @c nextVreg is passed
 * explicitly so that the functions are independent of the virtual-register
 * count of any particular function.
 */

#ifndef REGALLOC_UTILS_H
#define REGALLOC_UTILS_H

#include "block.h"
#include "instr_selector.h"

/* =========================================================================
 * Spill-cost weighting
 * ========================================================================= */

/**
 * @brief Return the spill cost weight for a given static loop-nesting depth.
 *
 * Uses the classic 10^depth heuristic: a variable live inside one loop is
 * 10× more expensive to spill than one outside; inside two nested loops, 100×;
 * and so on.  The depth is clamped to [0, 5] to avoid overflow.
 *
 * @param loopDepth  Static nesting depth of the instruction (from IRInstr.loopDepth).
 * @return           Spill weight (1, 10, 100, 1 000, 10 000, or 100 000).
 */
int regalloc_spill_weight(int loopDepth);

/* =========================================================================
 * Operand extraction
 * ========================================================================= */

/**
 * @brief Return the single register defined by @p in, or -1 if none.
 *
 * Instructions that do not write a destination (CMP, TEST, branches, CALL,
 * RET, PUSH, STORE, CQO, labels) return -1.  Physical-register destinations
 * are normalised: %al is mapped to %rax (PHYS_RAX) since they share the same
 * architectural register.
 *
 * @param in       Machine instruction to inspect.
 * @param nextVreg Base offset for physical-register ids (unused here; kept
 *                 for API symmetry with the other extraction functions).
 * @return         Virtual or physical register id of the destination, or -1.
 */
int instr_def(const MachInstr *in, int nextVreg);

/**
 * @brief Collect the explicit source registers read by @p in.
 *
 * Fills @p out with the virtual/physical register ids of all operands that
 * @p in reads explicitly.  For STORE, PUSH, IDIV, and CQO the destination
 * field is also a source (read-modify-write or implicit input), so it is
 * included in the use set.
 *
 * @param in       Machine instruction to inspect.
 * @param nextVreg Base offset for physical-register ids.
 * @param out      Output array; must hold at least LIVENESS_MAX_IDS entries.
 * @param n        Set to the number of ids written into @p out.
 */
void instr_uses(const MachInstr *in, int nextVreg, int out[], int *n);

/**
 * @brief Collect the implicit source registers read by @p in.
 *
 * Covers architectural implicit reads not visible in the MachOperand fields:
 *   - IDIV  : reads RAX (dividend low) and RDX (dividend high)
 *   - CQO   : reads RAX (to sign-extend into RDX)
 *   - CALL  : reads all caller-saved registers (ABI: callee may read them
 *              as arguments, though we model this conservatively)
 *   - RET   : reads RAX (return value)
 *
 * @param in       Machine instruction to inspect.
 * @param nextVreg Base offset for physical-register ids.
 * @param out      Output array; must hold at least LIVENESS_MAX_IDS entries.
 * @param n        Set to the number of ids written into @p out.
 */
void instr_implicit_uses(const MachInstr *in, int nextVreg, int out[], int *n);

/**
 * @brief Collect the implicit destination registers written by @p in.
 *
 * Covers architectural implicit writes not visible in the MachOperand fields:
 *   - IDIV  : writes RAX (quotient) and RDX (remainder)
 *   - CQO   : writes RDX (sign extension of RAX)
 *   - CALL  : clobbers all caller-saved registers
 *
 * @param in       Machine instruction to inspect.
 * @param nextVreg Base offset for physical-register ids.
 * @param out      Output array; must hold at least LIVENESS_MAX_IDS entries.
 * @param n        Set to the number of ids written into @p out.
 */
void instr_implicit_defs(const MachInstr *in, int nextVreg, int out[], int *n);

/**
 * @brief Collect all destination registers defined by @p in (wrapper).
 *
 * Calls @c instr_def() and writes at most one entry into @p out.  Provided
 * for API uniformity with @c instr_uses() at call sites that iterate over
 * both use and def sets.
 *
 * @param in       Machine instruction to inspect.
 * @param nextVreg Base offset for physical-register ids.
 * @param out      Output array; must hold at least 1 entry.
 * @param n        Set to 0 (no def) or 1.
 */
void instr_defs(const MachInstr *in, int nextVreg, int out[], int *n);

/* =========================================================================
 * Instruction predicates
 * ========================================================================= */

/**
 * @brief Return non-zero if @p op is a read-modify-write opcode.
 *
 * For RMW instructions the destination register is also an implicit source.
 * The spill inserter (ra_spill.c) uses this to emit a reload of the
 * destination slot before the instruction, rather than only a store after it.
 *
 * @param op  Machine opcode to test.
 * @return    1 if the destination is both read and written, 0 otherwise.
 */
int regalloc_is_rmw(MachOp op);

/**
 * @brief Return non-zero if @p op is a SETcc opcode.
 *
 * SETcc instructions write their result into %al (PHYS_AL).  The interference
 * graph builder uses this to add an exclusion for PHYS_RAX at the liveness
 * point after the SETcc, preventing the allocator from assigning a virtual
 * register to %rax while it is implicitly holding the SETcc result.
 *
 * @param op  Machine opcode to test.
 * @return    1 if @p op is one of SETE/SETNE/SETL/SETLE/SETG/SETGE, 0 otherwise.
 */
int regalloc_is_setcc(MachOp op);

/**
 * @brief Return non-zero if @p op transfers control flow.
 *
 * The spill inserter invalidates its within-block reload cache after every
 * control-transfer instruction because the next instruction may be reached
 * from a different predecessor with a different register state.
 *
 * @param op  Machine opcode to test.
 * @return    1 if @p op is a conditional branch, unconditional jump, call, or
 *            return; 0 otherwise.
 */
int regalloc_is_ctrl_transfer(MachOp op);

#endif /* REGALLOC_UTILS_H */