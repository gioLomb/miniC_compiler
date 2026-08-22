#ifndef RA_COLOR_H
#define RA_COLOR_H

#include "interference.h"
#include "bucket.h"
#include "ra_coalesce.h"

/*
 * Chaitin-Briggs graph coloring: Simplify + Select.
 *
 * ra_simplify(): rimuove nodi dal grafo in ordine bucket-by-degree,
 *                usando Briggs-optimistic per i potenziali spill.
 *                Restituisce lo stack di nodi da colorare.
 *
 * ra_select_colors(): assegna colori (registri fisici) in ordine inverso
 *                     di rimozione. Applica biased coloring: se un partner
 *                     move-related (da MoveList) ha un colore disponibile,
 *                     lo preferisce per eliminare MOV ridondanti.
 *                     Nodi senza colore disponibile vanno in spilled[].
 *                     Preferisce callee-saved per live-across-call.
 *
 * Il chiamante (regalloc.c) alloca e libera stack/spilled.
 * ml può essere NULL: in quel caso la selezione è identica alla versione
 * senza coalescing.
 */
int ra_simplify    (IGraph *g, int nextVreg, int **outStack);
int ra_select_colors(IGraph *g, int nextVreg, int *stack, int stackLen,
                     int *spilled, const MoveList *ml);

#endif /* RA_COLOR_H */