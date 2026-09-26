

#ifndef SVN_H
#define SVN_H


/**
 * @file svn.h
 * @brief Superlocal Value Numbering (SVN) optimisation pass on the linear IR.
 *
 * Performs value numbering across extended basic blocks, eliminating
 * redundant computations and enabling subsequent constant folding and
 * dead-code elimination.
 */

#include "ir.h"

/** Maximum names (leaders) tracked per value number in one scope chain. */
#define SVN_MAX_NAMES 4

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

