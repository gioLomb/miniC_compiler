#ifndef AST_TO_SYMTAB_H
#define AST_TO_SYMTAB_H

#include "parser/ast.h"
#include "symbol_table.h"

/*
 * Converte il nome testuale di un tipo, cosi' come compare nell'AST
 * (es. "int", "float"), nel corrispondente DataType. Le stringhe non
 * riconosciute vengono mappate su T_VOID (valore di fallback: l'unico
 * caso legittimo per T_VOID e' comunque il tipo di ritorno assente,
 * quindi non introduce ambiguita' pratiche per ora).
 */
DataType symtab_type_from_string(const char *typeName);

/*
 * PASS 1: popola 'global' con le signature di TUTTE le dichiarazioni
 * top-level dell'AST (figli diretti di ND_PROGRAM), senza scendere nei
 * Block dei corpi funzione. Serve a risolvere le forward reference tra
 * funzioni (una funzione puo' chiamarne un'altra dichiarata piu' avanti
 * nel file).
 *
 * Per ND_FUNC_DECL: registra un Symbol SYM_FUNC con dataType = tipo di
 * ritorno, paramCount/paramTypes ricavati dai nodi ND_PARAM (tutti i
 * figli tranne l'ultimo, che e' sempre il Block del corpo).
 * Per ND_VAR_DECL: registra un Symbol SYM_VAR (con isArray/arraySize se
 * e' un array).
 *
 * Restituisce il numero di errori di redeclaration incontrati (0 = nessuno).
 */
int symtab_populate_globals(ASTNode *program, Scope *global);

#endif
