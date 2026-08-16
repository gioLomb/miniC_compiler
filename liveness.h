/**
 * @file liveness.h
 * @brief Liveness analysis engine shared by DCE/LICM/SR (IR front-end)
 *        and register allocation / interference-graph construction
 *        (machine-code front-end).
 *
 * Architecture
 * ------------
 * A single backward dataflow engine (liveness_computeCore) drives both
 * front-ends.  The only front-end-specific part is how uses and defs are
 * extracted from a single instruction, expressed as a callback of type
 * LivenessExtractFn.  Two thin wrappers provide the public API:
 *
 *   liveness_computeIr()    — IR front-end (DCE, LICM, SR).
 *   liveness_computeMach()  — Machine-code front-end (regalloc).
 *
 * Bit-set representation
 * ----------------------
 * LiveSet è un typedef di BitSet (bitset.h): tipo primitivo generico.
 * I bit rappresentano indici di variabili/temporanei assegnati da VarMap.
 * loop.h usa BitSet direttamente con bit = indici di blocchi (dominatori).
 * Stesso layout, semantica distinta — il nome del tipo chiarisce il dominio.
 */

#ifndef LIVENESS_H
#define LIVENESS_H

#include <stdint.h>
#include "bitset.h"          /* tipo primitivo BitSet                  */
#include "ir.h"
#include "block.h"
#include "arena.h"
#include "varmap.h"
#include "regalloc_utils.h"

#define BITS_PER_WORD 64

typedef BitSet LiveSet;

typedef BitSetIter LiveSetIter;

#define LIVESET_ITER(s)          BITSET_ITER(s)
#define LIVESET_NEXT(it, out_id) BITSET_NEXT((it), (out_id))

/* =========================================================================
 * Generic backward dataflow engine
 * ========================================================================= */

#define LIVENESS_MAX_IDS 16

typedef void (*LivenessExtractFn)(void *ctx, int instrIdx,
                                   int uses[LIVENESS_MAX_IDS], int *nUses,
                                   int defs[LIVENESS_MAX_IDS], int *nDefs);

typedef struct {
    LiveSet *Use, *Def, *LiveIn, *LiveOut;
    int numVars, words;
} LivenessBlockSets;

LivenessBlockSets liveness_computeCore(int nBlocks, const BasicBlock *blocks,
                                         int numVars, const char *reachable,
                                         LivenessExtractFn extract, void *ctx,
                                         Arena *arena);

LiveSet *liveness_computePerInstr(int nBlocks, const BasicBlock *blocks,
                                     int instrCount, int numVars,
                                     const LiveSet *blockLiveOut,
                                     LivenessExtractFn extract, void *ctx,
                                     Arena *arena);

/* =========================================================================
 * Unified result type
 * ========================================================================= */
typedef struct {
    LivenessBlockSets blockSets;
    LiveSet          *liveAfter;
    VarMap            varMap;
} LivenessResult;

/* =========================================================================
 * Public front-end wrappers
 * ========================================================================= */
LivenessResult liveness_computeIr(IRFunction *f, const char *reachable,
                                    Arena *arena);

LivenessResult liveness_computeMach(const MachFunction *f,
                                      const BasicBlock *blocks,
                                      int nBlocks, Arena *arena);

#endif /* LIVENESS_H */