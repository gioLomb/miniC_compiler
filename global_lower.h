#ifndef GLOBAL_LOWER_H
#define GLOBAL_LOWER_H

#include "ir.h"
#include "arena.h"

#define IR_MAX_EXPANSION_FACTOR       3

/**
 * @file global_lower.h
 * @brief Lowering OPND_GLOBAL -> IR_GLOBAL_ADDR + IR_LOAD_ARR/IR_STORE_ARR.
 *
 * Eseguito una volta per funzione dopo ir_resolveCFG(), PRIMA di SVN/DCE/CP/LICM/SR.
 * Dopo questo pass, OPND_GLOBAL sopravvive solo come descrittore non-storage nel
 * src1 di IR_GLOBAL_ADDR: ogni altra passata e il backend trattano accessi globali
 * come qualsiasi accesso con base temporanea.
 *
 * Trasformazioni:
 *   Array globale usato come base di LOAD/STORE_ARR:
 *     dst = arr[idx]  ->  addr = GLOBAL_ADDR g; dst = addr[idx]
 *
 *   Scalare globale in lettura (src1 o src2):
 *     ... = g ...     ->  addr = GLOBAL_ADDR g; val = addr[0]; ... = val ...
 *
 *   Scalare globale in scrittura (dst di istruzione che definisce):
 *     g = ...         ->  addr = GLOBAL_ADDR g; tmp = ...; STORE_ARR addr[0] = tmp
 *
 * Nessuna cache dell'indirizzo tra istruzioni diverse: un temp addr emesso
 * dentro un branch non domina usi in altri path. SVN deduplica entro EBB,
 * LICM issa fuori dai loop — entrambi vedono IR_GLOBAL_ADDR come pura/invariante.
 */

/**
 * @brief Lower every OPND_GLOBAL operand of @p f into IR_GLOBAL_ADDR + LOAD/STORE_ARR.
 *
 * @param f      IR function rewritten in place (instrs/blocks arrays reallocated).
 * @param arena  Scratch arena for the temporary [oldIndex]->[newStart,newEnd)
 *               remap arrays used to realign block ranges after the
 *               instruction count changes. Pure scratch: never survives past
 *               this call, caller retains ownership and is free to reset it
 *               afterwards.
 */
void ir_lower_globals(IRFunction *f, Arena *arena);

#endif /* GLOBAL_LOWER_H */
