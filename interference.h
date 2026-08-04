#ifndef INTERFERENCE_H
#define INTERFERENCE_H

#include "bitset.h"
#include "instr_selector.h"
#include "regalloc_utils.h"   /* for RBlock */

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

IGraph ig_build(const MachFunction *f, const RBlock *blocks, int nBlocks,
                int nextVreg, const RSet *liveAfter);

void ig_add_edge(IGraph *g, int i, int j);
void ig_free(IGraph *g);
long tri_idx(int i, int j);
int ig_has_edge(const IGraph *g, int i, int j);

#endif