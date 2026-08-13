#ifndef PARSER_H
#define PARSER_H

#include "ast.h"
#include "../arena.h"

/*
 * Fa partire il parsing dell'intero programma (assume che lexer_open()
 * sia gia' stato chiamato).
 *
 * astArena  — arena in cui vengono allocati tutti i testi (node->text).
 *             Il chiamante la crea prima, la distrugge dopo freeAST().
 *             scratchArena (temporanea, interna) resta gestita dal parser.
 *
 * Restituisce la radice del parse tree.
 */
ASTNode *ParseProgram(Arena *astArena);

#endif