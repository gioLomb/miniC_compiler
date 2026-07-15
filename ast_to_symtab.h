#ifndef AST_TO_SYMTAB_H
#define AST_TO_SYMTAB_H

#include <stddef.h>
#include "arena.h"
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
 * Questa funzione la spacca nelle sue parti, scrivendo *outTypeName e
 * *outName come stringhe allocate in 'arena' (non in buffer forniti dal
 * chiamante): non c'e' quindi alcuna dimensione massima da rispettare,
 * qualunque sia la lunghezza reale del nome. E' condivisa da Pass 1 e
 * dal modulo semantico (semantic.c), che la riusa per estrarre nome/
 * tipo di ritorno di una funzione o per dichiarare variabili/parametri
 * nel proprio scope.
 *
 * 'arena' deve restare viva finche' outTypeName/outName servono: la
 * chiamata non fa alcuna copia oltre a quella nell'arena stessa.
 */
void symtab_parse_decl_text(Arena *arena, const char *text,
                             char **outTypeName, char **outName,
                             int *isArray, int *arraySize);

/*
 * Dichiara una singola variabile/parametro in 'scope', a partire dal
 * campo 'text' cosi' come lo produce il parser ("int x", "int arr[5]").
 * Usa 'arena' per il parsing intermedio (vedi symtab_parse_decl_text).
 * Riusata sia per ND_PARAM che per ND_VAR_DECL dal modulo semantico
 * (semantic.c), che la chiama mentre attraversa i corpi funzione
 * dichiarando variabili e risolvendo espressioni nello stesso giro.
 *
 * Restituisce 0 se 'name' e' gia' dichiarato in questo stesso scope
 * (redeclaration - errore semantico da segnalare al chiamante), 1
 * altrimenti.
 */
int symtab_declare_from_decl_text(Arena *arena, Scope *scope, const char *text);

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
 * Crea internamente una propria arena per il parsing intermedio delle
 * dichiarazioni, distrutta prima di ritornare: il chiamante non deve
 * gestirne il ciclo di vita.
 *
 * NOTA: la Pass 2 (scope dei parametri + corpi funzione) NON vive piu'
 * qui: e' stata fusa con l'analisi semantica in semantic.c, per evitare
 * di creare due alberi di scope paralleli. Vedi semantic.h.
 *
 * Restituisce il numero di errori di redeclaration incontrati (0 = nessuno).
 */
int symtab_populate_globals(ASTNode *program, Scope *global);

#endif
