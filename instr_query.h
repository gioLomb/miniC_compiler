#ifndef INSTR_QUERY_H
#define INSTR_QUERY_H

/**
 * @file instr_query.h
 * @brief Pure queries on MachInstr: operand extraction and opcode predicates.
 *
 * All register-id extraction is class-aware: pass RC_INT or RC_FLOAT so that
 * only operands belonging to that class are reported.  Physical ids are
 * class-local (classVregCount + localColor).
 */

#include "instr_selector.h"
#include "reg_class.h"

int  instr_def(const MachInstr *in, int classVregCount, RegClass cls);
void instr_uses(const MachInstr *in, int classVregCount, RegClass cls, int out[], int *n);
void instr_defs(const MachInstr *in, int classVregCount, RegClass cls, int out[], int *n);
void instr_implicit_uses(const MachInstr *in, int classVregCount, RegClass cls, int out[], int *n);
void instr_implicit_defs(const MachInstr *in, int classVregCount, RegClass cls, int out[], int *n);
int  instr_is_rmw(MachOpCode op);
int  instr_is_setcc(MachOpCode op);
int  instr_is_ctrl_transfer(MachOpCode op);

#endif /* INSTR_QUERY_H */
