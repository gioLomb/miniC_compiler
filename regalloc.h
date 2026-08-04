#ifndef REGALLOC_H
#define REGALLOC_H

#include "instr_selector.h"
#include "bitset.h"
#include "interference.h"
#include "regalloc_utils.h"

/*
 * Register allocation via graph coloring (Chaitin-Briggs, EaC 13.4).
 *
 * Precondizione: mp risultato di isel_select()+sched_schedule().
 * Postcondizione: ogni MO_VREG sostituito da MO_PHYS o MO_STACK; frameSize
 * riflette gli slot di spill; prologo/epilogo salvano i callee-saved usati.
 *
 * Tecniche: matrice bit + adjlist per il grafo di interferenza; Simplify
 * Briggs-optimistic con bucket-by-degree O(1) ammortizzato; spill cost
 * pesato per annidamento loop (10^loopDepth); cache di reload per riusare
 * lo stesso temporaneo su letture consecutive dello stesso slot spillato
 * entro un blocco (invalidata su label/salto/call).
 *
 * Limiti noti: nessun coalescing delle copie (EaC 13.4.3) — puo' restare
 * qualche MOV reg->reg ridondante dopo la colorazione. Corretto, solo
 * subottimale.
 */
void regalloc(MachProgram *mp);

#endif /* REGALLOC_H */