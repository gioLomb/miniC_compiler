#ifndef LICM_H
#define LICM_H

#include "ir.h"

/*
 * Loop-Invariant Code Motion (LICM) sull'IR lineare.
 *
 * Identifica i loop tramite back-edge nel CFG (arco B→H dove H domina B),
 * calcola le istruzioni invarianti con una worklist, verifica la sicurezza
 * dello spostamento, e muove le istruzioni sicure in un pre-header sintetico
 * inserito prima dell'header del loop.
 *
 * Un'istruzione dst = src1 op src2 e' invariante nel loop L se:
 *   - e' pura (nessun effetto collaterale)
 *   - per ogni operando sorgente: e' una costante, oppure e' definito
 *     esclusivamente fuori da L, oppure la sua unica definizione in L
 *     e' gia' marcata invariante (invarianza a cascata)
 *
 * Un'istruzione invariante puo' essere spostata nel pre-header solo se:
 *   - il blocco che la contiene domina tutte le uscite del loop
 *   - c'e' esattamente una definizione di dst nel loop
 *   - dst non e' live-in dell'header (nessun valore entrante da fuori
 *     che verrebbe oscurato dallo spostamento)
 *
 * Restituisce 1 se almeno un'istruzione e' stata spostata, 0 altrimenti.
 */
int licm_optimize(IRFunction *f);

#endif
