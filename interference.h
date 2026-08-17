#ifndef INTERFERENCE_H
#define INTERFERENCE_H

#include <stdbool.h>
#include "liveness.h"
#include "instr_selector.h"
#include "regalloc_utils.h"
#include "arena.h"
#include "dynamic_array.h"

#define MAX_INSTR_OPERANDS 16
#define MAX_EXPLICIT_DEFS 8
/* Historical name preserved: AdjList is now just an IntVector. */
typedef IntVector AdjList;

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
 * allocati in 'arena' (fornita dal caller); adj[i].data usa il modulo
 * dynamic_array e quindi una gestione dinamica separata dall'arena,
 * perché il numero di archi non è noto a priori.
 * L'arena deve restare viva finché l'IGraph è in uso.
 */
IGraph ig_build(const MachFunction *f, const BasicBlock *blocks, int nBlocks,
                int nextVreg, const LiveSet *liveAfter, Arena *arena);

/*
 * Libera solo i buffer dinamici di adj[i]. Gli array fissi sono nell'arena del
 * caller: vengono liberati da arena_destroy, già chiamata nel caller
 * subito dopo ig_free.
 */
void ig_free(IGraph *g);

#endif