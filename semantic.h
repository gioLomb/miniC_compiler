#ifndef SEMANTIC_H
#define SEMANTIC_H

#include "parser/ast.h"
#include "symbol_table.h"

/*
 * Analisi semantica: fonde in un UNICO attraversamento quello che prima
 * era la Pass 2 (creazione degli scope di parametri/blocchi + dichiara-
 * zione delle variabili locali) e la risoluzione/verifica degli usi
 * (ND_ID, ND_CALL, ND_ARRAY_ACCESS, assegnamenti, operatori, return).
 *
 * Perche' fuse in una sola funzione invece di due passate separate:
 * la Pass 2 "pura" apriva scope come variabili locali (fnScope,
 * blockScope dentro walkStmt) che sparivano non appena la funzione C
 * ritornava - nessun modo pulito per una fase successiva di recuperarli
 * dall'esterno per farci il lookup. Fondendo le due cose, lo scope in
 * cui si dichiara una variabile e quello in cui si risolve un suo uso
 * sono la STESSA variabile locale nella STESSA chiamata ricorsiva: non
 * serve mai esportare/recuperare nulla.
 *
 * Precondizione: 'global' deve essere gia' stato popolato con le
 * signature top-level da symtab_populate_globals() (Pass 1, in
 * ast_to_symtab.c/.h) - altrimenti le chiamate a funzioni dichiarate
 * "in avanti" nel file non si risolverebbero.
 *
 * Cosa verifica, nel dettaglio:
 *   - ND_ID / ND_ARRAY_ACCESS: il nome deve essere dichiarato nello
 *     scope corrente o in uno degli scope antenati (symtab_lookup);
 *     un ND_ID non puo' riferirsi a un array (va usato con []) ne' a
 *     una funzione; un ND_ARRAY_ACCESS richiede che il simbolo sia
 *     davvero un array e che l'indice sia di tipo int. Se l'indice e'
 *     una costante nota a compile-time (letterale, eventualmente con
 *     un meno unario), viene anche verificato che sia dentro i bound
 *     dell'array (0 <= indice < arraySize); un indice calcolato a
 *     runtime non puo' essere verificato qui (richiederebbe un
 *     controllo nel codice generato, fuori portata di un'analisi statica).
 *   - ND_CALL: il nome deve essere una funzione (non una variabile);
 *     il numero di argomenti deve combaciare con paramCount; ogni
 *     argomento deve essere assegnabile al tipo del parametro
 *     corrispondente (stessa regola di compatibilita' degli assegnamenti).
 *   - ND_ASSIGN: il lato sinistro dev'essere un ND_ID o ND_ARRAY_ACCESS
 *     (il parser accetterebbe sintatticamente anche "1 = 2;": qui viene
 *     rifiutato); l'unica conversione implicita ammessa e' int -> float
 *     (widening), il contrario (float -> int, narrowing) e' sempre errore.
 *   - ND_BINOP '%': richiede entrambi gli operandi int.
 *   - ND_RETURN: il tipo dell'espressione restituita deve essere
 *     assegnabile al tipo di ritorno dichiarato per la funzione
 *     corrente (stessa regola di compatibilita' int -> float).
 *
 * Gli scope creati (uno per funzione, uno per ogni ND_BLOCK annidato)
 * restano appesi all'albero radicato in 'global', esattamente come
 * accadeva con la vecchia Pass 2: nessuna distruzione qui dentro, se ne
 * occupa simtab_destroy_tree(global) a fine compilazione.
 *
 * Restituisce il numero totale di errori semantici incontrati (0 =
 * nessuno).
 */
int semantic_check(ASTNode *program, Scope *global);

#endif