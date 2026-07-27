#ifndef LOOP_H
#define LOOP_H

#include "ir.h"
#include "liveness.h"
#include "arena.h"

/*
 * Strutture e funzioni condivise per l'analisi dei loop,
 * usate sia da LICM che da Strength Reduction.
 */

#define MAX_LOOPS 64

typedef struct {
    int  header;
    int  preHeader;   /* -1 se non ancora creato */
    int *body;        /* array di indici di blocco nel loop body */
    int  bodyCount;
    int  exits[64];   /* blocchi dentro il loop con succ fuori */
    int  exitCount;
} Loop;

/* Calcola la matrice dei dominatori: Dom[b] = bitset dei blocchi che
 * dominano b. Allocato nell'arena. 'words' = ceil(nBlocks/64). */
LiveSet *loop_compute_dominators(IRFunction *f, int words, Arena *arena);

/* 1 se A domina B, 0 altrimenti. */
int loop_dominates(LiveSet *Dom, int a, int b);

/* Trova tutti i loop nel CFG tramite back-edge.
 * Scrive in loops[] (max MAX_LOOPS) e restituisce il conteggio. */
int loop_find(IRFunction *f, LiveSet *Dom, Loop *loops, Arena *arena);

/* Crea il pre-header per il loop L, aggiungendo un IRBlock in coda
 * e reindirizzando i predecessori esterni dell'header.
 * Restituisce l'indice del nuovo blocco. */
int loop_build_pre_header(IRFunction *f, Loop *L);

#endif
