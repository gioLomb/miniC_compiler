#ifndef OPTIMIZE_H
#define OPTIMIZE_H

#include "parser/ast.h"
#include "arena.h"

#define INT_BUF_SIZE   (CHAR_BIT * sizeof(long) / 3 + 3)
#define FLOAT_BUF_SIZE (DECIMAL_DIG + 8)
#define OP_KEY(c1, c2) ((unsigned short)(((unsigned char)(c1) << 8) | (unsigned char)(c2)))

/*
 * Ottimizzazioni AST-level (constant folding, pruning, balancing).
 * Eseguita DOPO semantic_check(), PRIMA di ir_generate().
 *
 * astArena — stessa arena passata a ParseProgram(): i nodi letterali
 *            creati durante il folding vi allocano i propri text,
 *            garantendo durata coerente con il resto dell'AST.
 */
void optimize_ast(ASTNode *program, Arena *astArena);

#endif