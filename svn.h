/**
 * @file svn.h
 * @brief Superlocal Value Numbering (SVN) optimisation pass on the linear IR.
 *
 * SVN assigns a compact integer *value number* to each IR operand and
 * expression, then rewrites every re-computation of an already-seen
 * expression as a plain copy:
 *
 *   t1 = a + b
 *   ...
 *   t2 = a + b   →   t2 = t1
 *
 * "Already seen" is defined along an *Extended Basic Block* (EBB): a
 * maximal chain of basic blocks where every block (except the first) has
 * exactly one predecessor.  Along such a chain every block is guaranteed
 * to have been executed before its successor, so a value computed earlier
 * is still valid later.  At a *join point* (a block with more than one
 * incoming edge) the chain is broken and a fresh scope is started, because
 * different predecessors may not have computed the same expressions.
 *
 * Scope stacking ("sheaf of tables")
 * ------------------------------------
 * Each block along an EBB pushes a new SVNScope on entry and pops it on
 * exit.  Lookups walk the stack from innermost to outermost, so values
 * defined in a dominating block are visible in dominated successors without
 * copying.  On scope pop, the child scope is destroyed; the parent scope
 * is never mutated, preserving the invariant that a sibling EBB branch
 * sees only the values available at the join point.
 *
 * The stale-leader problem
 * ------------------------
 * Because irExprInto() can write an expression result directly into a
 * named variable (not only into a fresh temporary), the same variable may
 * serve as the *leader* (canonical name) for a value and later be
 * overwritten.  Invalidating the leader eagerly on reassignment would
 * require mutating parent scopes and break the sheaf invariant.  Instead,
 * every candidate leader is validated lazily at the point of use via
 * svn_leaderStillValid(): the variable's current value number is looked up and
 * compared to the number for which it was registered.  Temporaries are
 * immune — they are written exactly once by construction.
 *
 * Known limitations (missed optimisations, not correctness issues)
 * -----------------------------------------------------------------
 * - IR_LOAD_ARR and IR_CALL are never memoised.  Array loads require alias
 *   analysis to rule out intervening IR_STORE_ARR; calls may have
 *   side-effects or return different values on each invocation.
 * - At most SVN_MAX_NAMES names are tracked per value number.  Excess names
 *   are silently dropped (no incorrect code is produced).
 * - Copies introduced by SVN ("t2 = t1") are not further propagated here;
 *   that is deferred to the CP pass.
 *
 * svn_optimize() is called by ir_buildFunction() immediately after CFG
 * construction and before any other pass.
 */

#ifndef SVN_H
#define SVN_H

#include "ir.h"

/**
 * @brief Run Superlocal Value Numbering on every EBB of function @p f.
 *
 * Walks the CFG starting from the entry block (index 0).  Blocks that
 * belong to an EBB (predCount == 1) are visited recursively within the
 * same scope chain; join points (predCount > 1) restart a fresh top-level
 * scope.  Already-visited blocks are skipped via a visited[] flag.
 *
 * The pass rewrites @p f->instrs in place: no new instructions are added,
 * but the opcode and operands of redundant computations are replaced with
 * IR_ASSIGN from the leader temporary.
 *
 * @param f IR function to optimise (modified in place).
 */
void svn_optimize(IRFunction *f);

#endif /* SVN_H */
