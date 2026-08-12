#ifndef RA_COLOR_H
#define RA_COLOR_H

#include "interference.h"

/*
 * Chaitin-Briggs graph coloring: Simplify + Select.
 *
 * simplify(): rimuove nodi dal grafo in ordine bucket-by-degree,
 *             usando Briggs-optimistic per i potenziali spill.
 *             Restituisce lo stack di nodi da colorare.
 *
 * select_colors(): assegna colori (registri fisici) in ordine inverso
 *                  di rimozione. Nodi senza colore disponibile vanno in
 *                  spilled[]. Preferisce callee-saved per live-across-call.
 *
 * Il chiamante (regalloc.c) alloca e libera stack/spilled.
 */
int  ra_simplify    (IGraph *g, int nextVreg, int **outStack);
int  ra_select_colors(IGraph *g, int nextVreg, int *stack, int stackLen,
                      int *spilled);

#endif /* RA_COLOR_H */