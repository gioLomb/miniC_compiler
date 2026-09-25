#ifndef AST_OPTIMIZER_H
#define AST_OPTIMIZER_H

/**
 * @file ast_optimizer.h
 * @brief AST-level algebraic identities and associative-chain balancing.
 *
 * Literal-literal folding (3+2, 1<2, ...) is intentionally not done here:
 * IR constant propagation (cp.c) is the single engine for that.
 * This pass still rewrites x+0 / x*1 / 0&&y and rebalances int + and * chains.
 */

#include "parser/ast.h"
#include "arena.h"

/** snprintf buffer for an integer literal (sign + digits + NUL). */
#define INT_BUF_SIZE   32
/** snprintf buffer for a floating-point literal. */
#define FLOAT_BUF_SIZE 64

/**
 * Pack two operator characters into a 16-bit key for O(1) switch dispatch.
 * Single-character operators use 0 as the second character (e.g. OP_KEY('+', 0)).
 */
#define OP_KEY(c0, c1) ((unsigned short)(((unsigned char)(c0) << 8) | (unsigned char)(c1)))

/**
 * @brief Rewrite @p root in place: algebraic identities plus int additive/multiplicative rebalancing.
 *
 * Does not fold two numeric literals; those remain for CP on the IR.
 *
 * @param root  ND_PROGRAM (or any subtree) produced by the parser + semantic pass.
 * @param arena Arena used for any newly allocated replacement nodes.
 */
void optimize_ast(ASTNode *root, Arena *arena);

#endif /* AST_OPTIMIZER_H */