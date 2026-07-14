#ifndef AST_TO_SYMTAB_H
#define AST_TO_SYMTAB_H

#include <stddef.h>
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
 * Il parser (vedi ParseDeclaration/ParseParamList in parser.c) memorizza
 * le dichiarazioni come un'unica stringa nel campo 'text' del nodo:
 *   "int x"        -> variabile semplice
 *   "int arr[5]"   -> array
 *   "int somma"    -> nome di funzione (tipo di ritorno + nome)
 * Questa funzione la spacca nelle sue parti. E' condivisa da Pass 1 (qui
 * sotto) e dal modulo semantico (semantic.c), che la riusa per estrarre
 * nome/tipo di ritorno di una funzione o per dichiarare variabili/
 * parametri nel proprio scope: la logica di parsing e' unica, un solo
 * punto dove puo' esserci un bug (es. usare l'intero 'text' invece del
 * solo nome come chiave di lookup - errore gia' capitato in passato).
 */
void symtab_parse_decl_text(const char *text,
                             char *typeName, size_t typeCap,
                             char *name, size_t nameCap,
                             int *isArray, int *arraySize);

/*
 * Dichiara una singola variabile/parametro in 'scope', a partire dal
 * campo 'text' cosi' come lo produce il parser ("int x", "int arr[5]").
 * Riusata sia per ND_VAR_DECL che per ND_PARAM, sia da questo modulo
 * (non piu', dato che la Pass 2 e' stata spostata) sia dal modulo
 * semantico (semantic.c), che la chiama mentre attraversa i corpi
 * funzione dichiarando variabili e risolvendo espressioni nello stesso
 * giro (vedi semantic.h per il perche' di questa fusione).
 *
 * Restituisce 0 se 'name' e' gia' dichiarato in questo stesso scope
 * (redeclaration - errore semantico da segnalare al chiamante), 1
 * altrimenti.
 */
int symtab_declare_from_decl_text(Scope *scope, const char *text);

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
 * NOTA: la Pass 2 (scope dei parametri + corpi funzione) NON vive piu'
 * qui: e' stata fusa con l'analisi semantica in semantic.c, per evitare
 * di creare due alberi di scope paralleli (uno per la sola costruzione,
 * uno per la risoluzione) che nessuno dei due potrebbe riusare dall'altro
 * senza esportare puntatori fragili. Vedi semantic.h.
 *
 * Restituisce il numero di errori di redeclaration incontrati (0 = nessuno).
 */
int symtab_populate_globals(ASTNode *program, Scope *global);

#endif
