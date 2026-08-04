#ifndef REGALLOC_UTILS_H
#define REGALLOC_UTILS_H

#include "instr_selector.h"

/* CFG block structure (used by interference and regalloc) */
typedef struct {
    int start, end;
    int succ[2];
} RBlock;

/* Utility functions */
int regalloc_normalize_phys(int p);
int regalloc_spill_weight(int loopDepth);
int regalloc_operand_reg(const MachOperand *o);
int regalloc_operand_reg2(const MachOperand *o);

/* Instruction analysis */
int instr_def(const MachInstr *in, int nextVreg);                /* returns single def or -1 */
void instr_uses(const MachInstr *in, int nextVreg, int out[], int *n);
void instr_implicit_uses(const MachInstr *in, int nextVreg, int out[], int *n);
void instr_implicit_defs(const MachInstr *in, int nextVreg, int out[], int *n);
void instr_defs(const MachInstr *in, int nextVreg, int out[], int *n); /* fills array, sets n */

/* Predicates */
int regalloc_is_rmw(MachOp op);
int regalloc_is_setcc(MachOp op);
int regalloc_is_ctrl_transfer(MachOp op);

#endif