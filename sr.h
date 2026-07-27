#ifndef SR_H
#define SR_H

#include "ir.h"

/*
 * Strength Reduction: sostituisce moltiplicazioni per la variabile
 * induttiva con addizioni piu' economiche.
 *
 * Prerequisito: licm_optimize() deve essere gia' stato chiamato sullo
 * stesso IRFunction, cosi' il pre-header esiste e le istruzioni
 * invarianti sono gia' state spostate fuori dal loop.
 *
 * Per ogni loop con variabile induttiva base i (i = i + c) e derivata
 * t = i * d, trasforma:
 *
 *   [pre-header]          [pre-header]
 *                   →       t_sr = i * d
 *
 *   [body]                [body]
 *     t = i * d     →       t = t_sr        (eliminato da DCE)
 *     i = i + c             i = i + c
 *                           t_sr = t_sr + (c*d)
 *
 * Restituisce 1 se almeno una trasformazione e' avvenuta, 0 altrimenti.
 */
int sr_optimize(IRFunction *f);

#endif
