#ifndef OPTIMIZE_H
#define OPTIMIZE_H

#include "parser/ast.h"

#define INT_BUF_SIZE  (CHAR_BIT * sizeof(long) / 3 + 3)
#define FLOAT_BUF_SIZE (DECIMAL_DIG + 8)
#define OP_KEY(c1, c2) ((unsigned short)(((unsigned char)(c1) << 8) | (unsigned char)(c2)))
/*
 * Ottimizzazioni a livello di AST, eseguite DOPO semantic_check() e PRIMA
 * della generazione dell'IR lineare. Muta l'albero in place (puo'
 * sostituire interi sottoalberi con nodi piu' semplici, o rimuovere
 * interi statement provatamente morti).
 *
 * Perche' dopo semantic_check() e non prima: il folding delle costanti
 * qui dentro NON ha bisogno di informazioni di tipo risolte (lavora solo
 * su coppie di nodi gia' letterali, il cui tipo e' evidente dal loro
 * stesso NodeKind) - potrebbe quindi girare anche prima. Il motivo vero
 * per farlo dopo e' un altro: potare un ramo morto (es. il ramo 'then'
 * di un "if (0)") PRIMA di aver fatto il type-check/name-resolution
 * significherebbe non controllare mai quel codice - un eventuale nome
 * non dichiarato dentro un ramo irraggiungibile sparirebbe silenziosamente
 * invece di essere segnalato. Prima si valida tutto (anche il codice
 * morto), poi lo si pota.
 *
 * Un limite noto di questo ordinamento: il bound-check statico sugli
 * array in semantic.c riconosce solo indici GIA' letterali al momento in
 * cui gira (es. "arr[3]"), non espressioni foldabili come "arr[2+1]" -
 * dato che il folding qui avviene DOPO quel controllo, un'espressione
 * come "arr[2+1]" viene comunque semplificata a "arr[3]" (utile per l'IR),
 * ma non beneficia piu' del controllo statico dei bound su quel valore
 * gia' risolto. Non e' un bug di correttezza (il codice generato resta
 * corretto), solo un'occasione mancata di diagnostica a compile-time.
 *
 * Le quattro ottimizzazioni implementate (le uniche per cui l'AST e' lo
 * strumento giusto - le altre della lista, come CSE via DAG, sono
 * rimandate all'IR lineare):
 *
 *   1. Constant folding di superficie: se un ND_BINOP/ND_UNARY ha
 *      operandi gia' letterali (ND_NUM_INT/ND_NUM_FLOAT), lo sostituisce
 *      col letterale risultato. La divisione/modulo per costante zero
 *      NON viene foldata (si lascia al runtime, che fallira' com'e'
 *      giusto che faccia, invece di inventare un valore a compile-time).
 *
 *   2. Semplificazioni algebriche, SOLO su interi (x+0, 0+x, x-0, x*1,
 *      1*x, x*0, 0*x): sui float alcune di queste sarebbero matematicamente
 *      scorrette in IEEE754 (es. x*0.0 non e' 0 se x e' NaN o -Inf), quindi
 *      sono limitate a operandi/letterali interi. Il caso x*0/0*x e' l'unico
 *      che elide la valutazione dell'altro operando: viene applicato SOLO
 *      se quell'operando e' provatamente privo di effetti collaterali
 *      (vedi hasSideEffect piu' sotto) - altrimenti l'operazione resta
 *      inalterata, per non eliminare una chiamata, una scrittura, un
 *      accesso ad array o una divisione che potrebbero avere un effetto
 *      osservabile (inclusi un crash o un trap a runtime).
 *
 *   3. Pruning strutturale di rami/loop provatamente morti: se la
 *      condizione di un ND_IF/ND_WHILE, dopo il folding, risulta un
 *      letterale, il ramo mai eseguito viene rimosso interamente
 *      (deallocato) e il nodo di controllo sostituito dal solo ramo
 *      superstite (o rimosso del tutto, se non c'e' nulla da eseguire).
 *
 *   4. Array bounds check elimination: gia' implementata in semantic.c
 *      (bound-check statico sugli indici costanti) - non c'e' altro da
 *      fare qui, la lista la cita solo per completezza.
 *
 *   5. Tree height balancing su catene '+' e '*': una catena associativa a
 *      sinistra come "((a+b)+c)+d" (la forma naturale prodotta da un
 *      parser ricorsivo-discendente per operatori associativi a sinistra)
 *      viene appiattita e ricostruita bilanciata, per ridurre la
 *      lunghezza della catena di dipendenze nell'IR risultante (piu'
 *      parallelizzabile/schedulabile). Limitato a '+' e '*' su soli
 *      interi: '-'/'/' non sono associativi cosi' come sono scritti, e
 *      per i float l'arrotondamento IEEE754 non e' associativo (riordinare
 *      la somma puo' cambiare il risultato nell'ultimo bit).
 *
 *      LIMITE NOTO: per essere certi che una catena sia di soli interi
 *      servirebbe il tipo delle variabili coinvolte - optimize.c non ha
 *      pero' alcun accesso alla symbol table (nessuno Scope* nella sua
 *      firma). La guardia qui e' quindi puramente strutturale: rifiuta
 *      la catena solo se un letterale float e' VISIBILE nel sottoalbero
 *      (vedi containsFloatLiteral in optimize.c). Una catena di sole
 *      variabili float, senza alcun letterale in vista, non viene
 *      riconosciuta e verrebbe bilanciata comunque - risolvibile solo
 *      estendendo optimize_ast con un vero Scope* (rimandato, non
 *      implementato in questa versione).
 *
 *      L'appaiamento e' per livelli (come costruire un min-heap da un
 *      array), non tramite una coda a priorita': assume costo/altezza
 *      uniforme delle foglie. Resta corretto anche quando una foglia e'
 *      una sotto-espressione molto piu' profonda delle altre, solo non
 *      ottimale in quel caso (occasione mancata, non un bug).
 */
void optimize_ast(ASTNode *program);

#endif