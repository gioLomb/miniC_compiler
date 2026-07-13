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
 * Popola l'intero albero di scope a partire da 'program' (ND_PROGRAM) e
 * 'global' (scope radice, gia' creato dal chiamante con scope_create(NULL)).
 *
 * Internamente esegue due passate in sequenza (Pass 1 poi Pass 2), ma dal
 * punto di vista di chi chiama questa funzione la popolazione della symtab
 * e' un'unica operazione:
 *
 *   Pass 1 (firma delle dichiarazioni top-level)
 *     Per ogni figlio diretto di 'program', registra la signature in
 *     'global' senza scendere nei corpi funzione: per ND_FUNC_DECL un
 *     Symbol SYM_FUNC (tipo di ritorno + paramCount/paramTypes dai nodi
 *     ND_PARAM, tutti i figli tranne l'ultimo, che e' sempre il Block),
 *     per ND_VAR_DECL un Symbol SYM_VAR. Questo risolve le forward
 *     reference tra funzioni (una funzione puo' chiamarne un'altra
 *     dichiarata piu' avanti nel file).
 *
 *   Pass 2 (scope dei parametri + corpi funzione)
 *     Per ogni ND_FUNC_DECL gia' registrato in Pass 1, crea uno scope
 *     figlio di 'global', vi dichiara i parametri (ND_PARAM) e poi
 *     attraversa ricorsivamente il corpo (l'ultimo figlio, un ND_BLOCK)
 *     dichiarando ogni ND_VAR_DECL incontrato nello scope giusto. Apre
 *     un nuovo scope figlio solo in corrispondenza di ND_BLOCK; ND_IF e
 *     ND_WHILE ricorrono sui rami senza introdurre un proprio scope (uno
 *     statement senza graffe non e' un blocco lessicale a se'). Non fa
 *     alcun symtab_lookup sugli usi (ND_ID/ND_CALL/ND_ARRAY_ACCESS): quello
 *     e' compito di una fase successiva (analisi semantica).
 *
 * Gli scope creati restano appesi all'albero radicato in 'global' (non
 * vengono distrutti singolarmente): il chiamante libera tutto in un colpo
 * solo con symtab_destroy_tree(global) a fine compilazione.
 *
 * Restituisce il numero totale di errori di redeclaration incontrati nelle
 * due passate (0 = nessuno).
 */
int symtab_populate(ASTNode *program, Scope *global);

#endif