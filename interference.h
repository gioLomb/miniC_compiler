#ifndef INTERFERENCE_H
#define INTERFERENCE_H

#include "liveness.h"
#include "instr_selector.h"
#include "regalloc_utils.h"

typedef struct {
    int *data;
    int  len;
    int  cap;
} AdjList;

typedef struct {
    int       n;
    uint64_t *matrix;
    AdjList  *adj;
    int      *degree;
    int      *color;
    int      *active;
    uint32_t *excl;
    int      *spillCost;
    char     *crossesCall;
} IGraph;

/* Riceve BasicBlock* (ex RBlock*) */
IGraph ig_build(const MachFunction *f, const BasicBlock *blocks, int nBlocks,
                int nextVreg, const LiveSet *liveAfter);

void ig_free(IGraph *g);

#endif