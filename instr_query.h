#ifndef INSTR_QUERY_H
#define INSTR_QUERY_H

/**
 * @file instr_query.h
 * @brief Pure queries on MachInstr: operand extraction and opcode predicates.
 *
 * Class-aware counterpart of regalloc_utils.h: every extraction function
 * takes a @c RegClass and only reports operands of that class. Ids are
 * class-local: vregs in [0, classVregCount), physicals in
 * [classVregCount, classVregCount + <class reg count>).
 */

#include "instr_selector.h"
#include "reg_class.h"

/**
 * @brief Register of class @p cls defined by @p in, or -1.
 * @param classVregCount  Physical-id base offset for @p cls.
 */
int instr_def(const MachInstr *in, int classVregCount, RegClass cls);

/**
 * @brief Collect source registers of class @p cls read by @p in.
 * @param out  Output array, sized by caller for worst case.
 * @param n    Number of ids written.
 */
void instr_uses(const MachInstr *in, int classVregCount, RegClass cls, int out[], int *n);

/**
 * @brief Wrapper around instr_def() writing at most one entry into @p out.
 */
void instr_defs(const MachInstr *in, int classVregCount, RegClass cls, int out[], int *n);

/**
 * @brief Collect implicit ABI reads of class @p cls (CALL/IDIV/CQO/RET).
 */
void instr_implicit_uses(const MachInstr *in, int classVregCount, RegClass cls, int out[], int *n);

/**
 * @brief Collect implicit ABI writes of class @p cls (CALL/IDIV/CQO).
 */
void instr_implicit_defs(const MachInstr *in, int classVregCount, RegClass cls, int out[], int *n);

/**
 * @brief 1 if @p op is read-modify-write (dst also a source).
 */
int instr_is_rmw(MachOpCode op);

/**
 * @brief 1 if @p op is any SETcc variant.
 */
int instr_is_setcc(MachOpCode op);

/**
 * @brief 1 if @p op transfers control flow (jump/call/ret).
 */
int instr_is_ctrl_transfer(MachOpCode op);

#endif /* INSTR_QUERY_H */