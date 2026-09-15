#ifndef GLOBAL_LOWER_H
#define GLOBAL_LOWER_H


/**
 * @file global_lower.h
 * @brief Module for lowering global variable accesses in the IR.
 *
 * Rewrites loads and stores of global variables into explicit address
 * calculations and memory operations that the later instruction-selection
 * and register-allocation phases can handle uniformly.
 */

#include "ir.h"
#include "arena.h"

#define IR_MAX_EXPANSION_FACTOR 7

/**
 * @brief Lower every OPND_GLOBAL operand of @p f into IR_GLOBAL_ADDR + LOAD/STORE_ARR.
 *
 * @param f      IR function rewritten in place (instrs/blocks arrays reallocated).
 * @param arena  Scratch arena for the temporary [oldIndex]->[newStart,newEnd)
 *               remap arrays used to realign block ranges after the
 *               instruction count changes. Pure scratch: never survives past
 *               this call, caller retains ownership and is free to reset it
 *               afterwards.
 */
void gl_lower_globals(IRFunction *f, Arena *arena);

#endif /* GLOBAL_LOWER_H */
