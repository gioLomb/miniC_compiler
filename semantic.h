#ifndef SEMANTIC_H
#define SEMANTIC_H


/**
 * @file semantic.h
 * @brief Single-pass semantic analysis: scope creation, name resolution, and type checking.
 */

#include "parser/ast.h"
#include "symbol_table.h"

int semantic_check(ASTNode *program, Scope *global);

#endif
