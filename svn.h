#ifndef SVN_H
#define SVN_H

#include "ir.h"

/*
 * SVN (Scoped Value Numbering) – ottimizzazione a livello di IR lineare.
 * Riconosce espressioni duplicate all'interno di una stessa funzione
 * e le sostituisce con il risultato già calcolato, usando una sheaf di
 * tabelle per gestire correttamente gli scope (blocchi) e le ridefinizioni
 * di variabili. Gestisce anche il caso di variabili con nome che vengono
 * riassegnate (vedi validità del leader al momento del riuso).
 *
 * Precondizione: IRFunction deve essere già generato da ir_generate().
 * Modifica l'IR in-place, trasformando alcune istruzioni in semplici
 * copie (IR_ASSIGN) da un leader ancora valido.
 */
void svn_optimize(IRFunction *f);

#endif