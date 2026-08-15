#ifndef CP_H
#define CP_H

#include "ir.h"

/*
 * Constant Propagation + CFG Pruning sull'IR lineare.
 *
 * Usa il framework delle Reaching Definitions in versione semplificata:
 * per ogni variabile/temporaneo, traccia se ha un unico valore costante
 * noto su TUTTI i cammini che raggiungono un dato punto (must-be-constant),
 * oppure se il suo valore e' indeterminato (piu' definizioni diverse la
 * raggiungono) o ignoto (nessuna definizione costante la raggiunge).
 *
 * Algoritmo (forward dataflow, iterativo a punto fisso):
 *   - Ogni blocco ha una ConstMap in ingresso (In) e in uscita (Out).
 *   - In[b] = meet di tutti i Out[pred(b)]: se tutti i predecessori
 *     concordano su un valore costante per una variabile, In[b] la
 *     conosce come costante; se almeno uno discorda, la marca come
 *     indeterminata (TOP -> costante -> BOTTOM).
 *   - Out[b] = In[b] aggiornato dalle istruzioni del blocco: un
 *     IR_ASSIGN con sorgente costante propaga la costante; qualunque
 *     altra definizione azzera la costante per quella variabile.
 *   - Converge perche' i valori scendono nella reticolo (da TOP a
 *     costante, da costante a BOTTOM) e non risalgono mai.
 *
 * Dopo la convergenza, ogni istruzione che legge una variabile con
 * valore costante noto viene riscritta sostituendo la variabile con
 * la costante inline. Se una IR_IF_FALSE legge una condizione costante,
 * viene sostituita con IR_GOTO (ramo preso) o rimossa (ramo non preso),
 * e succ[]/predCount del CFG vengono aggiornati (CFG Pruning).
 *
 * Restituisce 1 se l'IR e' stato modificato (per il ciclo a punto
 * fisso esterno in ir_buildFunction()), 0 altrimenti.
 */
int cp_optimize(IRFunction *f);

#endif
