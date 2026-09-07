/**
 * @file cp.h
 * @brief Constant Propagation + CFG Pruning on the linear IR.
 *
 * This pass combines two tightly coupled transformations that are most
 * effective when run together:
 *
 * Constant Propagation
 * --------------------
 * Tracks, for every variable and temporary, whether it holds a single
 * known constant value on ALL paths that reach a given program point.
 * Uses a three-element lattice per variable:
 *
 *   LAT_UNKNOWN  (⊤) — no assignment has been seen yet (initial state)
 *   LAT_CONST       — every reaching definition assigns the same constant
 *   LAT_CONFLICT (⊥) — two or more different values reach this point
 *
 * Meet operator at join points: UNKNOWN meet X = X; CONST(a) meet CONST(b)
 * = CONST(a) if a==b else CONFLICT; CONFLICT meet anything = CONFLICT.
 * Values only descend in the lattice, guaranteeing termination.
 *
 * After the forward dataflow reaches a fixed point, every variable use
 * that maps to LAT_CONST is replaced inline with the constant value.
 * Binary and unary operations whose operands are both constant after
 * substitution are further folded into a single IR_ASSIGN.
 *
 * CFG Pruning
 * -----------
 * Two structural simplifications that eliminate dead control-flow edges:
 *
 *  - IR_IF_FALSE with a constant condition is replaced by IR_GOTO (branch
 *    always taken) or deleted (branch never taken); the unreachable
 *    successor's predCount is decremented.
 *
 *  - GOTO / IF_FALSE that targets the immediately following instruction
 *    (jump-to-next) is deleted; it contributes no reachable edge.
 *
 *  - IR_LABEL nodes that no remaining jump references are deleted (they
 *    become orphans after CFG pruning removes their referencing jumps).
 *
 * Integration
 * -----------
 * cp_optimize() returns 1 if it modified the IR, 0 otherwise.  The caller
 * (ir_buildFunction in ir.c) runs CP and DCE in a loop until both return 0,
 * because constant folding can expose new dead code, and DCE can expose
 * new constants by removing conflicting definitions.
 *
 * Dependencies: constmap.h (lattice + ConstMap), varmap.h (operand ids),
 *               arena.h (all dataflow storage), ir.h (IRFunction).
 */

#ifndef CP_H
#define CP_H

#include "ir.h"
#include "varmap.h"

/**
 * @brief Run constant propagation and CFG pruning on a single IR function.
 *
 * Executes five internal passes in sequence:
 *
 *  1. Build VarMap — assign a compact integer id to every distinct
 *     variable and temporary that appears in @p f.
 *
 *  2. Allocate ConstMaps — one in[b] and one out[b] per basic block,
 *     all initialised to LAT_UNKNOWN (⊤).
 *
 *  3. Forward dataflow — iterate until no out[b] changes:
 *       in[b]  = meet of out[p] for all CFG predecessors p of b
 *       out[b] = transfer function applied to in[b] over b's instructions
 *
 *  4. Rewrite — scan each block with a local copy of in[b]:
 *       - substitute variable uses with known constants
 *       - fold binary/unary instructions whose operands became constant
 *       - prune IR_IF_FALSE and jump-to-next edges from the CFG
 *       - mark eliminated instructions in an eliminate[] boolean array
 *
 *  5. Sweep — compact f->instrs by removing eliminated instructions;
 *     update block [start, end) ranges to match the new positions.
 *
 * All dataflow storage (ConstMaps, VarMap backing) is allocated from a
 * temporary arena that is destroyed before returning.
 *
 * @param f    IR function to optimise (modified in place).
 * @param vm   Shared operand->id VarMap, owned by the caller (ir.c). Reused
 *             across every call in the CP/DCE fixed-point loop instead of
 *             being rebuilt each time; cp_optimize only registers any
 *             not-yet-seen operand (get-or-create, cheap when already known).
 * @param arenaScratch  Scratch arena for all dataflow storage (reset here).
 * @return     1 if @p f was modified, 0 if the IR was already at fixed point.
 */
int cp_optimize(IRFunction *f, VarMap *vm, Arena *arenaScratch);

#endif /* CP_H */
