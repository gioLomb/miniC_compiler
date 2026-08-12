#ifndef INTERFERENCE_H
#define INTERFERENCE_H

#include <stdbool.h>
#include "liveness.h"
#include "instr_selector.h"
#include "regalloc_utils.h"
#include "arena.h"

typedef struct {
    int  len;
    int  cap;
    int *data;   /* malloc/realloc: cresce dinamicamente in ig_add_edge */
} AdjList;

typedef struct {
    int       n;
    uint64_t *matrix;
    AdjList  *adj;
    int      *degree;
    int      *color;
    bool     *active;      /* solo 0/1: bool permette memset e risparmia memoria */
    uint32_t *excl;
    int      *spillCost;
    char     *crossesCall;
} IGraph;

/*
 * Costruisce il grafo di interferenza. Tutti gli array fissi vengono
 * allocati in 'arena' (fornita dal caller); adj[i].data usa malloc/realloc
 * separato perché cresce per numero di archi non noto a priori.
 * L'arena deve restare viva finché l'IGraph è in uso.
 */
IGraph ig_build(const MachFunction *f, const BasicBlock *blocks, int nBlocks,
                int nextVreg, const LiveSet *liveAfter, Arena *arena);

/*
 * Libera solo adj[i].data (malloc). Gli array fissi sono nell'arena del
 * caller: vengono liberati da arena_destroy, già chiamata nel caller
 * subito dopo ig_free.
 */
void ig_free(IGraph *g);

#endif