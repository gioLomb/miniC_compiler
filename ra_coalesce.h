#ifndef RA_COALESCE_H
#define RA_COALESCE_H

#include "instr_selector.h"
#include "interference.h"

/*
 * Biased coloring (coalescing leggero, senza modifica al grafo).
 *
 * Per ogni MACH_MOV tra due vreg (o vreg e fisico) non interferenti,
 * registra la coppia. ra_select_colors userà queste coppie come "hint":
 * se il partner ha già un colore disponibile (dentro il mask valido),
 * lo preferisce — eliminando il MOV ridondante senza toccare IGraph.
 *
 * Correttezza garantita: hint usato SOLO se il colore è già in `available`,
 * cioè supera tutti i vincoli (interferenza, excl[], crossesCall).
 */

typedef struct {
    int u;   /* vreg id (< nextVreg) */
    int v;   /* vreg id oppure nextVreg+physReg per registri fisici */
} MovePair;

typedef struct {
    MovePair *pairs;
    int       count;
    int       cap;
} MoveList;

/* Raccoglie coppie move-related da MachFunction; alloca con malloc.
 * Solo MOV vreg<->vreg e vreg<->phys, non interferenti (ig_has_edge). */
MoveList ra_collect_moves(const MachFunction *f, const IGraph *g, int nextVreg);

void movelist_free(MoveList *ml);

#endif /* RA_COALESCE_H */
