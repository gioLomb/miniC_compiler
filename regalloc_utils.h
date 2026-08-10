#ifndef REGALLOC_UTILS_H
#define REGALLOC_UTILS_H

#include "block.h"
#include "instr_selector.h"

/*
 * RBlock rimosso: usare BasicBlock (block.h) direttamente.
 * Le funzioni che prima ricevevano RBlock* ora ricevono BasicBlock*.
 */

/* Utility functions */
int regalloc_spill_weight(int loopDepth);


/* Instruction analysis */
int  instr_def(const MachInstr *in, int nextVreg);
void instr_uses(const MachInstr *in, int nextVreg, int out[], int *n);
void instr_implicit_uses(const MachInstr *in, int nextVreg, int out[], int *n);
void instr_implicit_defs(const MachInstr *in, int nextVreg, int out[], int *n);
void instr_defs(const MachInstr *in, int nextVreg, int out[], int *n);

/* Predicates */
int regalloc_is_rmw(MachOp op);
int regalloc_is_setcc(MachOp op);
int regalloc_is_ctrl_transfer(MachOp op);

#endif