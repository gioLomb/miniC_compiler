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
 *                              Builds a VarMap (Operand → compact int id)
 *                              and delegates to liveness_computeCore.
 *                              Does NOT compute per-instruction liveness
 *                              (the IR passes only need block-level sets).
 *
 *   liveness_computeMach()  — Machine-code front-end (regalloc).
 *                              Re-uses the same dataflow engine but also
 *                              calls liveness_computePerInstr() to
 *                              produce the liveAfter[] array required by
 *                              the interference-graph builder.
 *
 * LivenessBlock (formerly a separate type) has been removed; BasicBlock
 * (block.h) is used directly throughout.
 *
 * Bit-set representation
 * ----------------------
 * Variables are identified by compact integer ids produced by VarMap.
 * Each block-level liveness set (Use, Def, LiveIn, LiveOut) is a LiveSet —
 * a flat array of uint64_t words, one bit per variable id.  All LiveSet
 * instances for a given analysis are allocated from a caller-supplied Arena
 * and are freed in a single arena_destroy() call.
 */

#ifndef LIVENESS_H
#define LIVENESS_H

#include <stdint.h>
#include "ir.h"
#include "block.h"
#include "arena.h"
#include "varmap.h"
#include "regalloc_utils.h"

#define BITS_PER_WORD 64

/* =========================================================================
 * LiveSet — the project's single bit-set type
 * =========================================================================
 * All bit-set operations are inlined for performance; the words array is
 * allocated from the caller's arena, so no per-set free() is needed.
 * ========================================================================= */

/**
 * @brief A fixed-width bit set used to represent variable liveness.
 *
 * @c bits  points to @c words consecutive uint64_t values allocated in an
 * arena.  Bit @c id is in @c bits[id>>6], at position @c (id & 63).
 */
typedef struct {
    uint64_t *bits;
    int       words;
} LiveSet;

/**
 * @brief Allocate a zero-initialised LiveSet of @p words 64-bit words from @p arena.
 *
 * @param arena Arena from which the bit array is allocated.
 * @param words Number of uint64_t words (ceil(numVars / 64)).
 * @return      A LiveSet whose @c bits array is ready for use.
 */
LiveSet liveset_new(Arena *arena, int words);

/**
 * @brief Set bit @p id in @p s (mark variable @p id as live).
 *
 * @param s  Target set.
 * @param id Variable id to set; must be in [0, s->words * 64).
 */
void liveset_set(LiveSet *s, int id);

/**
 * @brief Clear bit @p id in @p s (mark variable @p id as dead).
 *
 * @param s  Target set.
 * @param id Variable id to clear; must be in [0, s->words * 64).
 */
void liveset_clrbit(LiveSet *s, int id);

/**
 * @brief Test whether bit @p id is set in @p s.
 *
 * @param s  Set to query.
 * @param id Variable id to test.
 * @return   Non-zero if the bit is set, zero otherwise.
 */
int liveset_test(const LiveSet *s, int id);

/**
 * @brief Return non-zero if @p a and @p b contain identical bit patterns.
 *
 * Both sets must have the same @c words count (enforced by the caller).
 *
 * @param a First set.
 * @param b Second set.
 * @return  1 if equal, 0 otherwise.
 */
int liveset_equal(const LiveSet *a, const LiveSet *b);

/**
 * @brief Copy all bits from @p src into @p dst (shallow word-level copy).
 *
 * @p dst and @p src must have the same @c words count.
 *
 * @param dst Destination set (overwritten).
 * @param src Source set (unchanged).
 */
void liveset_copy(LiveSet *dst, const LiveSet *src);

/* =========================================================================
 * LiveSetIter — forward iterator over set bits
 * -------------------------------------------------------------------------
 * Replaces the old LIVESET_FOREACH / LIVESET_FOREACH_END macro pair with a
 * single-macro initialiser + a next-step predicate, keeping parentheses
 * balanced and making the loop body look like ordinary C:
 *
 *   int id;
 *   for (LiveSetIter it = LIVESET_ITER(s); LIVESET_NEXT(&it, &id); )
 *       ... use id ...
 * ========================================================================= */

/**
 * @brief Opaque iterator state for traversing the set bits of a LiveSet.
 *
 * Initialise with @c LIVESET_ITER and advance with @c LIVESET_NEXT.
 * Do not access the fields directly.
 */
typedef struct {
    const LiveSet *s;
    int            word;   /**< Index of the current uint64_t word being scanned. */
    uint64_t       bits;   /**< Remaining bits in the current word.               */
} LiveSetIter;

/**
 * @brief Initialise a LiveSetIter over @p s.
 *
 * Usage:
 * @code
 *   int id;
 *   for (LiveSetIter it = LIVESET_ITER(s); LIVESET_NEXT(&it, &id); )
 *       // use id
 * @endcode
 *
 * @param s  LiveSet to iterate (must remain valid for the iterator's lifetime).
 */
#define LIVESET_ITER(s) \
    { (s), 0, ((s)->words > 0 ? (s)->bits[0] : 0ULL) }

/**
 * @brief Advance @p it to the next set bit and write its id into @p out_id.
 *
 * @param it      Pointer to a LiveSetIter previously initialised with LIVESET_ITER.
 * @param out_id  Pointer to the int that receives the next live variable id.
 * @return        1 if a live id was written into @p out_id, 0 if the set is exhausted.
 */
#define LIVESET_NEXT(it, out_id) \
    (liveset_iter_next_impl((it), (out_id)))

/**
 * @brief Internal step function for LIVESET_NEXT — do not call directly.
 *
 * Uses @c __builtin_ctzll to locate the lowest set bit in O(1), then clears
 * it so the next call advances past it.
 *
 * @param it      Iterator state (modified in place).
 * @param out_id  Receives the id of the next live variable.
 * @return        1 if an id was produced, 0 when all words are exhausted.
 */
static inline int liveset_iter_next_impl(LiveSetIter *it, int *out_id) {
    while (it->bits == 0) {
        it->word++;
        if (it->word >= it->s->words) return 0;
        it->bits = it->s->bits[it->word];
    }
    int b    = __builtin_ctzll(it->bits);
    *out_id  = (it->word << 6) + b;
    it->bits &= it->bits - 1;   // clear lowest set bit
    return 1;
}

/* =========================================================================
 * Generic backward dataflow engine
 * ========================================================================= */

/** Maximum number of use/def ids that a single instruction can expose. */
#define LIVENESS_MAX_IDS 16

/**
 * @brief Callback type for extracting uses and defs from instruction @p instrIdx.
 *
 * The callback writes compact variable ids (already mapped through VarMap
 * or an equivalent scheme) into @p uses / @p defs and sets @p nUses / @p nDefs.
 *
 * @param ctx       Opaque context pointer (front-end specific).
 * @param instrIdx  Flat instruction index into the function's instruction array.
 * @param uses      Output array for use ids (capacity LIVENESS_MAX_IDS).
 * @param nUses     Set to the number of ids written into @p uses.
 * @param defs      Output array for def ids (capacity LIVENESS_MAX_IDS).
 * @param nDefs     Set to the number of ids written into @p defs.
 */
typedef void (*LivenessExtractFn)(void *ctx, int instrIdx,
                                   int uses[LIVENESS_MAX_IDS], int *nUses,
                                   int defs[LIVENESS_MAX_IDS], int *nDefs);

/**
 * @brief Per-block Use/Def sets and the LiveIn/LiveOut result of the dataflow.
 *
 * All arrays are allocated from the caller's arena and share a single
 * backing allocation for the bit words, keeping memory locality high.
 */
typedef struct {
    LiveSet *Use, *Def, *LiveIn, *LiveOut;
    int numVars, words;
} LivenessBlockSets;

/**
 * @brief Run the backward liveness dataflow to a fixed point.
 *
 * Computes Use[b] and Def[b] for each block by scanning its instructions
 * through the @p extract callback, then iterates the standard equations:
 *
 *   LiveOut[b] = union of LiveIn[s] for all successors s of b
 *   LiveIn[b]  = Use[b] ∪ (LiveOut[b] − Def[b])
 *
 * All LiveSet storage is allocated from @p arena.
 *
 * @param nBlocks   Number of basic blocks.
 * @param blocks    Array of BasicBlock descriptors (start/end/succ).
 * @param numVars   Total number of tracked variable ids.
 * @param reachable Optional per-block reachability mask; NULL = all reachable.
 * @param extract   Callback to extract uses/defs from one instruction.
 * @param ctx       Context pointer forwarded to every @p extract call.
 * @param arena     Arena for all output LiveSet allocations.
 * @return          Populated LivenessBlockSets.
 */
LivenessBlockSets liveness_computeCore(int nBlocks, const BasicBlock *blocks,
                                         int numVars, const char *reachable,
                                         LivenessExtractFn extract, void *ctx,
                                         Arena *arena);

/**
 * @brief Compute per-instruction liveness (liveAfter[i]) from block-level LiveOut.
 *
 * Performs a single backward sweep over each block, propagating liveness
 * instruction by instruction.  Required by the interference-graph builder,
 * which needs to know exactly which variables are live after each machine
 * instruction.
 *
 * @param nBlocks       Number of basic blocks.
 * @param blocks        Block descriptors.
 * @param instrCount    Total number of instructions.
 * @param numVars       Number of tracked variable ids.
 * @param blockLiveOut  LiveOut sets from liveness_computeCore.
 * @param extract       Use/def extraction callback.
 * @param ctx           Context forwarded to @p extract.
 * @param arena         Arena for the output array.
 * @return              Heap (arena) array of @p instrCount LiveSet values;
 *                      liveAfter[i] holds ids live immediately after instr i.
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
 * @brief Combined result returned by both public front-end wrappers.
 *
 * @c liveAfter is NULL when computed through the IR front-end (not needed
 * by IR-level passes).  @c varMap is populated only by the IR front-end;
 * the machine front-end manages variable ids externally.
 *
 * Ownership: all LiveSet data lives in the arena passed to the compute
 * function; varMap owns its hash table and must be destroyed with
 * varmap_destroy() before the arena is released.
 */
typedef struct {
    LivenessBlockSets blockSets;
    LiveSet          *liveAfter;   /**< per-instruction sets; NULL for IR front-end */
    VarMap            varMap;      /**< Operand→id map; only valid for IR front-end */
} LivenessResult;

/* =========================================================================
 * Public front-end wrappers
 * ========================================================================= */

/**
 * @brief IR-level liveness analysis (used by DCE, LICM, SR).
 *
 * Builds a VarMap over all Operands in @p f, then runs the backward
 * dataflow.  Does not compute per-instruction liveness.
 *
 * @param f          IR function to analyse.
 * @param reachable  Optional reachability mask; NULL = all blocks reachable.
 * @param arena      Arena for all output allocations.
 * @return           LivenessResult with blockSets populated and liveAfter=NULL.
 */
LivenessResult liveness_computeIr(IRFunction *f, const char *reachable,
                                    Arena *arena);

/**
 * @brief Machine-code liveness analysis (used by regalloc and interference).
 *
 * Runs the backward dataflow over machine instructions and additionally
 * computes liveAfter[] for every instruction in the function.
 *
 * @param f       Machine function to analyse.
 * @param blocks  CFG built by regalloc (BasicBlock array).
 * @param nBlocks Number of blocks.
 * @param arena   Arena for all output allocations.
 * @return        LivenessResult with both blockSets and liveAfter populated.
 */
LivenessResult liveness_computeMach(const MachFunction *f,
                                      const BasicBlock *blocks,
                                      int nBlocks, Arena *arena);

#endif