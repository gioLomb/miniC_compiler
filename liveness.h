

#ifndef LIVENESS_H
#define LIVENESS_H


/**
 * @file liveness.h
 * @brief Liveness analysis engine shared by DCE, LICM, SR (IR front-end) and register allocation / interference-graph construction (machine-code front-end).
 *
 * Computes live-in / live-out sets for every instruction or basic block.
 * The same core algorithm is reused both on the linear IR and on the
 * machine-instruction representation.
 */

#include <stdint.h>
#include "bitset.h"
#include "ir.h"
#include "block.h"
#include "arena.h"
#include "varmap.h"
#include "instr_query.h"

/** Number of bits per uint64_t word in a bit-set. */
#define BITS_PER_WORD 64

/**
 * @brief A per-block or per-instruction liveness bit-set.
 *
 * Aliased to BitSet for domain clarity: when used in liveness.h/liveness.c
 * each bit represents a *variable or temporary id* (as assigned by VarMap).
 * The same underlying BitSet type is used in loop.h for dominator sets, where
 * each bit represents a *block index* — same layout, different semantics.
 */
typedef BitSet LiveSet;

/** Iterator type alias for traversing a LiveSet's set bits. */
typedef BitSetIter LiveSetIter;

/**
 * @brief Initialise a LiveSet iterator over @p s.
 *
 * Usage:
 * @code
 *   int id;
 *   for (LiveSetIter it = LIVESET_ITER(&live); LIVESET_NEXT(&it, &id); )
 *       // id is the next live variable id
 * @endcode
 */
#define LIVESET_ITER(s)          BITSET_ITER(s)

/**
 * @brief Advance a LiveSetIter and store the next live variable id in @p out_id.
 *
 * @return Non-zero while there are more live variable ids; 0 when exhausted.
 */
#define LIVESET_NEXT(it, out_id) BITSET_NEXT((it), (out_id))

/* =========================================================================
 * Generic backward dataflow engine
 * ========================================================================= */

/**
 * @brief Maximum number of use or def ids extractable from a single instruction.
 *
 * Conservative upper bound used to size the stack arrays inside
 * LivenessExtractFn callbacks.  For IR instructions the maximum is 2 (src1,
 * src2); for machine instructions implicit ABI reads (e.g. all 9 caller-saved
 * regs for CALL) can reach PHYS_CALLER_SAVED_COUNT = 9.
 */
#define LIVENESS_MAX_IDS 16

/**
 * @brief Callback type that extracts use and def ids from a single instruction.
 *
 * The engine calls this once per instruction per block during the Use/Def
 * construction pass.  The callback must fill:
 *   - uses[0..nUses-1] with ids of operands *read* by instruction instrIdx.
 *   - defs[0..nDefs-1] with ids of operands *written* by instruction instrIdx.
 * All ids must be non-negative integers compatible with the bit-set width.
 *
 * @param ctx       Opaque context pointer (front-end-specific data).
 * @param instrIdx  Index of the instruction to analyse.
 * @param uses      Output array for use ids; capacity LIVENESS_MAX_IDS.
 * @param nUses     Set to the number of ids written into uses[].
 * @param defs      Output array for def ids; capacity LIVENESS_MAX_IDS.
 * @param nDefs     Set to the number of ids written into defs[].
 */
typedef void (*LivenessExtractFn)(void *ctx, int instrIdx,
                                   int uses[LIVENESS_MAX_IDS], int *nUses,
                                   int defs[LIVENESS_MAX_IDS], int *nDefs);

/**
 * @brief Per-block liveness sets returned by the dataflow engine.
 *
 * All four arrays have length nBlocks; each entry is a LiveSet of numVars bits.
 * All bit words are allocated in a single contiguous slab from the caller's
 * arena for cache-friendly iteration during the fixed-point loop.
 */
typedef struct {
    LiveSet *Use;     /**< Use[b]: variables read in b before being defined.  */
    LiveSet *Def;     /**< Def[b]: variables defined in b.                    */
    LiveSet *LiveIn;  /**< LiveIn[b]: live at the entry of b.                 */
    LiveSet *LiveOut; /**< LiveOut[b]: live at the exit of b.                 */
    int numVars;      /**< Total number of tracked variable ids.              */
    int words;        /**< Number of uint64_t words per LiveSet (= ⌈numVars/64⌉). */
} LivenessBlockSets;

/**
 * @brief Run the backward liveness dataflow to a fixed point.
 *
 * Computes Use[], Def[], LiveIn[], and LiveOut[] for every block via the
 * standard iterative algorithm.  Blocks marked unreachable in @p reachable
 * are skipped entirely (neither Use/Def built nor updated in the fixed-point
 * loop).
 *
 * All output sets are allocated from @p arena; they remain valid until the
 * arena is destroyed.
 *
 * @param nBlocks  Number of basic blocks.
 * @param blocks   Array of BasicBlock descriptors (start, end, succ[]).
 * @param numVars  Number of distinct variable ids (bit-set width).
 * @param reachable Per-block reachability flags; NULL = all blocks reachable.
 * @param extract  Callback to extract use/def ids from one instruction.
 * @param ctx      Opaque context forwarded to @p extract.
 * @param arena    Arena for all output allocations.
 * @return         Fully populated LivenessBlockSets.
 */
LivenessBlockSets liveness_computeCore(int nBlocks, const BasicBlock *blocks,
                                         int numVars, const char *reachable,
                                         LivenessExtractFn extract, void *ctx,
                                         Arena *arena);

/**
 * @brief Compute per-instruction liveAfter[] sets via a single backward sweep.
 *
 * Requires that block-level liveOut[] is already at the fixed point
 * (call liveness_computeCore first).  For each instruction i,
 * liveAfter[i] holds the set of variable ids live *immediately after* i
 * has executed — i.e., the set a definition at i must not alias.
 *
 * Used exclusively by the machine-code front-end (interference graph
 * construction requires per-instruction granularity).
 *
 * @param nBlocks      Number of basic blocks.
 * @param blocks       BasicBlock array.
 * @param instrCount   Total number of instructions across all blocks.
 * @param numVars      Number of variable ids (bit-set width).
 * @param blockLiveOut LiveOut[] from liveness_computeCore().
 * @param extract      Use/def extraction callback.
 * @param ctx          Opaque context forwarded to @p extract.
 * @param arena        Arena for all output allocations.
 * @return             Array liveAfter[instrCount], arena-allocated.
 */
LiveSet *liveness_computePerInstr(int nBlocks, const BasicBlock *blocks,
                                     int instrCount, int numVars,
                                     const LiveSet *blockLiveOut,
                                     LivenessExtractFn extract, void *ctx,
                                     Arena *arena);

/* =========================================================================
 * Unified result type
 * ========================================================================= */

/**
 * @brief Combined result of a complete liveness analysis pass.
 *
 * - blockSets  : per-block Use/Def/LiveIn/LiveOut (always populated).
 * - liveAfter  : per-instruction live sets (only populated by the machine
 *                front-end; NULL for the IR front-end which does not need it).
 * - varMap     : operand → compact-id mapping used to build the bit-sets.
 *                When created privately (sharedVarMap == NULL), owns the
 *                VarMap and must be destroyed with varmap_destroy() before
 *                the arena is freed.
 */
typedef struct {
    LivenessBlockSets blockSets;  /**< Per-block liveness sets.              */
    LiveSet          *liveAfter;  /**< Per-instruction live sets, or NULL.   */
    VarMap           *varMap;     /**< Operand-to-id mapping (heap memory).  */
} LivenessResult;

/**
 * @param f             IR function to analyse.
 * @param reachable     Per-block reachability flags (NULL = all reachable).
 * @param sharedVarMap  Optional pre-existing VarMap to reuse. Pass NULL to
 *        keep the original behaviour: a fresh private VarMap is created and
 *        owned by the returned LivenessResult (caller must varmap_destroy it).
 * @param arena         Arena for all output allocations except varMap.
 * @return              LivenessResult; varMap must be destroyed by the
 *                       caller only when @p sharedVarMap was NULL.
 */
LivenessResult liveness_computeIr(IRFunction *f, const char *reachable,
                                    VarMap *sharedVarMap, Arena *arena);

/**
 * @brief Run liveness analysis on a machine-code function.
 *
 * Variable universe: vregs in [0, nextVreg) plus physical registers in
 * [nextVreg, nextVreg + PHYS_ALLOCATABLE).  Both are assigned fixed ids so
 * the interference graph can treat virtual and physical registers uniformly.
 *
 * Also computes per-instruction liveAfter[] (stored in LivenessResult.liveAfter)
 * needed by ig_build() to add edges between definitions and live-at-def vregs.
 *
 * @param f        Machine function to analyse.
 * @param blocks   CFG array produced by regalloc_build_cfg() in regalloc.c.
 * @param nBlocks  Number of entries in @p blocks.
 * @param arena    Arena for all output allocations except varMap.
 * @return         LivenessResult with liveAfter populated; varMap is NULL
 *                 (machine front-end does not use VarMap).
 */
LivenessResult liveness_computeMach(const MachFunction *f,
                                      const BasicBlock *blocks,
                                      int nBlocks, RegClass cls, int classVregCount,
                                      Arena *arena);

#endif /* LIVENESS_H */