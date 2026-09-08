#ifndef INSTR_QUERY_H
#define INSTR_QUERY_H

#include "block.h"
#include "instr_selector.h"

/**
 * @file instr_query.h
 * @brief Pure MachInstr property queries: operand extraction and opcode predicates.
 *
 * Layering rationale: these functions depend only on MachInstr/MachOperand
 * (instr_selector.h). They sit BELOW both the scheduler (sched.c/sched_dag.c,
 * phase 2) and the register allocator (regalloc.c/interference.c/ra_spill.c,
 * phase 3) in the pipeline isel -> sched -> regalloc, and both phases need
 * the same use/def/opcode information. Previously these lived in
 * regalloc_utils.h, making an earlier phase (sched) depend on a module
 * named/scoped for a later phase — backwards layering. Moving them here
 * removes that inversion: sched and regalloc both depend downward on this
 * module, never on each other's utilities.
 *
 * regalloc_spill_weight() stays in regalloc_utils.h: it is a genuinely
 * regalloc-only concept (spill-cost heuristic), not a property of the
 * instruction itself.
 */

/** @brief Return the single register defined by @p in, or -1 if none. */
int instr_def(const MachInstr *in, int nextVreg);

/** @brief Collect the explicit source registers read by @p in. */
void instr_uses(const MachInstr *in, int nextVreg, int out[], int *n);

/** @brief Collect the implicit source registers read by @p in (ABI reads). */
void instr_implicit_uses(const MachInstr *in, int nextVreg, int out[], int *n);

/** @brief Collect the implicit destination registers written by @p in (ABI writes). */
void instr_implicit_defs(const MachInstr *in, int nextVreg, int out[], int *n);

/** @brief Collect all destination registers defined by @p in (wrapper around instr_def). */
void instr_defs(const MachInstr *in, int nextVreg, int out[], int *n);

/** @brief Return non-zero if @p op is a read-modify-write opcode (dst also implicit source). */
int instr_is_rmw(MachOpCode op);

/** @brief Return non-zero if @p op is a SETcc opcode (writes %al). */
int instr_is_setcc(MachOpCode op);

/** @brief Return non-zero if @p op transfers control flow. */
int instr_is_ctrl_transfer(MachOpCode op);

#endif /* INSTR_QUERY_H */