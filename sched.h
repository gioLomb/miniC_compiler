#ifndef SCHED_H
#define SCHED_H

#include "instr_selector.h"

/*
 * Instruction Scheduler — Local List Scheduling per blocco base.
 *
 * Target: Intel Core i5 (architettura x86-64 Out-of-Order, Haswell/Broadwell).
 *
 * Obiettivo: riordinare le istruzioni dentro ogni blocco base per:
 *   1. Anticipare istruzioni ad alta latenza (IMUL, IDIV, LOAD, STORE)
 *      in modo che la CPU OoO trovi i valori pronti il prima possibile.
 *   2. Preservare le coppie CMP/TEST + Jcc adiacenti per favorire la
 *      macro-fusion del decodificatore Intel (due istruzioni → 1 μop).
 *   3. Rispettare tutte le dipendenze RAW, WAR, WAW per correttezza.
 *
 * Algoritmo: forward list scheduling con priorità = altezza del cammino
 * critico ponderato per latenza (longest latency-weighted path to a sink).
 *
 * Struttura interna: array di puntatori a nodi DAG (EaC §4.4.3) per
 * accesso casuale O(1) durante costruzione archi e scansione ready list.
 *
 * Limitazioni note:
 *   - Scheduling locale (per blocco base): ignora dipendenze inter-blocco.
 *   - Latenze stimate (tabelle Agner Fog Haswell): non tengono conto di
 *     cache miss, esecuzione speculativa, throughput vs latenza.
 *   - WAR/WAW su virtual register: trattate come dipendenze hard per
 *     correttezza; l'hardware le risolve con register renaming a runtime.
 */
void sched_schedule(MachProgram *mp);

#endif /* SCHED_H */