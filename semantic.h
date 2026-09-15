#ifndef SEMANTIC_H
#define SEMANTIC_H


/**
 * @file semantic.h
 * @brief Single-pass semantic analysis: scope creation, name resolution, and type checking.
 *
 * Walks the AST, builds nested symbol tables, resolves identifiers to their
 * declarations, checks type compatibility of expressions and statements, and
 * annotates the tree with the resulting type information.
 */

#include "parser/ast.h"
#include "symbol_table.h"

int semantic_check(ASTNode *program, Scope *global);

#endif
