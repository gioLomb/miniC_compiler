#ifndef PARSER_H
#define PARSER_H


/**
 * @file parser.h
 * @brief Recursive-descent parser interface.
 *
 * Consumes the token stream produced by the lexer and builds the Abstract
 * Syntax Tree according to the miniC grammar. Reports syntax errors through
 * the centralized error collector.
 */

#include "ast.h"
#include "../arena.h"

#define NEW_NODE(p, kind, text) newNode((p)->ast_arena, (kind), (text))

/**
 * @brief Parses an entire program and generates its corresponding AST representation.
 *
 * Expects that `lexer_open()` (or equivalent lexer initialization) has already been invoked.
 *
 * @param astArena Memory arena used to allocate permanent node text payloads (`node->text`).
 *                 The caller manages the lifetime of this arena (destroying it after calling `freeAST()`).
 * @return Pointer to the root ASTNode (`ND_PROGRAM`) of the generated syntax tree.
 */
ASTNode *ParseProgram(Arena *astArena);

#endif /* PARSER_H */
