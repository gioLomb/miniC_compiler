#ifndef OPTIMIZE_H
#define OPTIMIZE_H

#include "parser/ast.h"
#include "arena.h"

/**
 * Buffer size safety limit for string representation of long integers.
 */
#define INT_BUF_SIZE   (CHAR_BIT * sizeof(long) / 3 + 3)

/**
 * Buffer size safety limit for string representation of double precision floats.
 */
#define FLOAT_BUF_SIZE (DECIMAL_DIG + 8)

/**
 * Macro helper to encode one or two operator characters into a unique 16-bit key.
 */
#define OP_KEY(c1, c2) ((unsigned short)(((unsigned char)(c1) << 8) | (unsigned char)(c2)))

/**
 * @brief Performs high-level AST optimizations including constant folding, dead code elimination,
 *        and tree height balancing.
 *
 * @details Traverses the Abstract Syntax Tree (AST) to evaluate constant expressions at compile time,
 *          remove unreachable code blocks (such as dead branches in conditional statements or 
 *          unreachable while loops), and rebalance associative operator chains to reduce tree depth.
 *          
 *          Prerequisites:
 *          Must be called AFTER semantic analysis (`semantic_check()`) and BEFORE intermediate 
 *          representation generation (`ir_generate()`).
 *
 * @param program  Pointer to the root AST node representing the entire program.
 * @param astArena Pointer to the memory arena used for AST node allocations. Newly created
 *                 literal nodes allocate their string representations within this arena 
 *                 to guarantee lifecycle consistency with the rest of the tree.
 */
void optimize_ast(ASTNode *program, Arena *astArena);

#endif
