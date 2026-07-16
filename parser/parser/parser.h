#ifndef PARSER_H
#define PARSER_H

#include "ast.h"

/* Fa partire il parsing dell'intero programma (assume che lexer_open()
   sia gia' stato chiamato). Restituisce la radice del parse tree. */
ASTNode *ParseProgram(void);

#endif
