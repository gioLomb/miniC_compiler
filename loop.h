

#ifndef LOOP_H
#define LOOP_H


/**
 * @file loop.h
 * @brief Loop detection and pre-header construction shared by LICM and SR.
 */

#include "ir.h"
#include "bitset.h"
#include "arena.h"

/** Maximum number of loops tracked in a single function. */
#define MAX_LOOPS 64

/**
 * @brief Descriptor for a single natural loop.
 *
 * Populated by loop_find(); pre-header field is filled by
 * loop_build_pre_header() and initialised to -1 until then.
 */
typedef struct {
    int  header;        /**< Index of the loop-header block.                     */
    int  preHeader;     /**< Index of the synthetic pre-header (-1 if not yet created). */
    int *body;          /**< Arena-allocated array of block indices in the body.  */
    int  bodyCount;     /**< Number of entries in body[].                         */
    int  exits[64];     /**< Blocks inside loop with at least one out-of-loop successor. */
    int  exitCount;     /**< Number of entries in exits[].                        */
} Loop;

/**
 * @brief Compute the dominator sets for all blocks in @p f.
 *
 * Uses the iterative dataflow algorithm (intersection of predecessor Dom sets)
 * until a fixed point is reached.  The entry block (index 0) is initialised
 * to dom only by itself; all other blocks start as dominated by every block
 * (universal set) and the set shrinks on each iteration.
 *
 * @param f      IR function whose CFG is analysed.
 * @param words  Number of uint64_t words per bit-set (ceil(blockCount / 64)).
 * @param arena  Arena from which the Dom[] array is allocated.
 * @return       Array of BitSet, one per block; Dom[b] contains the indices
 *               of all blocks that dominate b.
 */
BitSet *loop_compute_dominators(IRFunction *f, int words, Arena *arena);

/**
 * @brief Return non-zero if block @p a dominates block @p b.
 *
 * Checks bit @p a in Dom[@p b]; O(1) via the bit-set representation.
 *
 * @param Dom  Dominator array produced by loop_compute_dominators().
 * @param a    Candidate dominator block index.
 * @param b    Block index to test.
 * @return     1 if a dominates b, 0 otherwise.
 */
int loop_dominates(BitSet *Dom, int a, int b);

/**
 * @brief Find all natural loops in @p f and populate @p loops[].
 *
 * Scans every CFG edge b→h; if h dominates b the edge is a back-edge and
 * defines a natural loop with header h.  The loop body is computed by a
 * backward BFS from b stopping at h (all blocks that can reach the back-edge
 * tail without leaving the loop).  Exit blocks (body blocks with at least
 * one successor outside the body) are also recorded for use by LICM.
 *
 * @param f      IR function to analyse.
 * @param Dom    Dominator sets from loop_compute_dominators().
 * @param loops  Output array of Loop descriptors; caller must provide space
 *               for at least MAX_LOOPS entries.
 * @param arena  Arena for body[] arrays inside each Loop.
 * @return       Number of loops found (0..MAX_LOOPS).
 */
int loop_find(IRFunction *f, BitSet *Dom, Loop *loops, Arena *arena);

/**
 * @brief Insert a synthetic pre-header block before @p L->header.
 *
 * Creates a new IRBlock at the end of f->blocks[] with a single successor
 * (the loop header) and no instructions (empty [start, end) range pointing
 * past the current last instruction).  Every predecessor of the header that
 * is NOT part of the loop body is re-routed to jump to the pre-header
 * instead; the header's predCount is adjusted accordingly.
 *
 * After this call L->preHeader holds the new block's index and the pre-header
 * is ready to receive hoisted or initialisation instructions.
 *
 * @param f  IR function (modified: new block appended, successor edges updated).
 * @param L  Loop whose pre-header is being created (L->preHeader updated).
 * @return   Index of the newly created pre-header block.
 */
int loop_build_pre_header(IRFunction *f, Loop *L);

#endif /* LOOP_H */