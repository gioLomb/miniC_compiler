#ifndef INSTR_QUERY_H
#define INSTR_QUERY_H


/**
 * @file instr_query.h
 * @brief Pure MachInstr property queries: operand extraction and opcode predicates.
 */

#include "block.h"
#include "instr_selector.h"

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