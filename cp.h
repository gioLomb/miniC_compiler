

#ifndef CP_H
#define CP_H


/**
 * @file cp.h
 * @brief Constant Propagation + CFG Pruning on the linear IR.
 *
 * Performs sparse conditional constant propagation, folds arithmetic and
 * comparison operations, and removes branches that have become constant,
 * thereby pruning unreachable basic blocks from the control-flow graph.
 */

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
 * @param vm   Shared operand-id VarMap, owned by the caller. Its
 *             per-instruction id cache (varmap_sync_cache) is refreshed
 *             here only if @p f changed structurally since the caller's
 *             last pass, replacing the previous per-call full rebuild.
 * @param arenaScratch  Scratch arena for all dataflow storage.
 * @return     1 if @p f was modified, 0 if the IR was already at fixed point.
 */
int cp_optimize(IRFunction *f, VarMap *vm, Arena *arenaScratch);

#endif /* CP_H */
