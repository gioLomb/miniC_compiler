#ifndef RA_SPILL_H
#define RA_SPILL_H

#include "instr_selector.h"

/*
 * Inserisce load/store per i virtual register spillati.
 *
 * Per ogni vreg in spilled[0..nSpilled):
 *   - alloca uno slot sullo stack (frameOff aumenta di 8 per ogni vreg)
 *   - sostituisce ogni uso  con un load  temporaneo dal slot
 *   - sostituisce ogni def  con uno store temporaneo nel slot
 *
 * Cache reload: entro un blocco base, riusa il temporaneo di reload per
 * letture consecutive dello stesso slot (invalidata a ogni label/salto/call).
 *
 * Modifica f->instrs in place (realloc interno); aggiorna *frameOff.
 */
void ra_spill_insert(MachFunction *f, const int *spilled, int nSpilled,
                     int *frameOff);

#endif /* RA_SPILL_H */
