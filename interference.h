

#ifndef INTERFERENCE_H
#define INTERFERENCE_H


/**
 * @file interference.h
 * @brief Interference graph (IGraph) construction for register allocation.
 */

#include <stdbool.h>
#include "liveness.h"
#include "instr_selector.h"
#include "reg_class.h"
#include "regalloc_utils.h"
#include "arena.h"
#include "dynamic_array.h"

#define IG_ADJ_INITIAL_CAPACITY 16

/** Maximum number of explicit def/use operands extracted per instruction. */
#define MAX_INSTR_OPERANDS 16

/** Maximum number of explicit destination operands per instruction. */
#define MAX_EXPLICIT_DEFS   8

/** Historical alias: adjacency lists are IntVectors. */
typedef IntVector AdjList;

/**
 * @brief Interference graph for a single machine function.
 *
 * All pointer fields except @c adj[i].data point into the caller's arena.
 * @c adj[i].data is heap-allocated by IntVector; release with @c ig_free().
 */
typedef struct {
    int       n;             /**< Total nodes: nextVreg + PHYS_ALLOCATABLE ([0,nextVreg)=virtual, rest=physical). */
    uint64_t *matrix;        /**< Lower-triangular bit matrix; pair (i,j), i>j, at bit i*(i-1)/2+j. */
    AdjList  *adj;           /**< Per-node adjacency list. */
    int      *degree;        /**< Current interference degree; decremented during Simplify. */
    int      *color;         /**< Assigned colour: -1=uncoloured, -2=spilled, >=0=phys reg index. */
    bool     *active;        /**< 1 while node still in the graph. */
    uint32_t *excl;          /**< Forbidden colour bitmask beyond interference edges (e.g. SETcc clobbers %al). */
    int      *spillCost;     /**< Loop-depth-weighted def/use count. */
    char     *crossesCall;   /**< 1 if live across a CALL; prefer callee-saved to cut push/pop. */
    char     *isReloadTemp;  /**< 1 if a reload/spill temp from a prior ra_spill_insert() round;
                              *   lets ra_simplify() avoid respilling short-lived temps first. */
} IGraph;

/**
 * @brief Test whether edge (i, j) exists in the interference graph.
 *
 * Converts the pair to a flat index, selects the correct uint64_t word
 * (idx >> 6 = idx / 64) and extracts the appropriate bit (idx & 63).
 */
int ig_has_edge(const IGraph *g, int i, int j);

/**
 * @brief Build the interference graph from the machine-code liveness result.
 *
 * Allocates all IGraph fields from @p arena, then populates edges and metadata
 * in a single forward scan over every instruction in every basic block:
 *
 *   - For each instruction @c i and each register @c d that @c i defines
 *     (explicitly via @c instr_defs, or implicitly via @c instr_implicit_defs),
 *     an interference edge (d, v) is added for every register @c v that is
 *     live in @c liveAfter[i].  This is the standard def-interferes-with-live
 *     rule: d and every live-at-definition v must reside in different registers.
 *
 *   - Spill costs are accumulated: each instruction contributes
 *     @c regalloc_spill_weight(loopDepth) to every vreg it explicitly
 *     uses or defines, so variables inside hot loops become expensive to spill.
 *
 *   - CALL instructions additionally set @c excl (forbid all caller-saved colours)
 *     and @c crossesCall for every vreg live after the call.
 *
 *   - IDIV and CQO add RAX/RDX to the @c excl mask of any live vreg, since those
 *     physical registers are implicitly clobbered by the instruction sequence.
 *
 *   - SETcc instructions add RAX to @c excl for live vregs, because SETcc writes
 *     %al (low byte of RAX) and a vreg in RAX would alias the result.
 *
 *   - Every node v with @p firstSpillVreg <= v < @p nextVreg has
 *     @c isReloadTemp[v] set to 1 (see module-level doc and IGraph.isReloadTemp).
 *
 * Physical registers are pre-coloured: color[nextVreg + p] = p for
 * p in [0, PHYS_ALLOCATABLE).  They are always active and participate in
 * interference edges so the allocator never assigns a conflicting colour.
 *
 * @pre  @p liveAfter must be the per-instruction liveness array produced by
 *       @c liveness_computeMach() for the same function and block layout.
 * @pre  The caller-supplied @p arena must remain live as long as the IGraph
 *       is in use (it owns all fixed-size arrays).
 *
 * @param f          Machine function to analyse.
 * @param blocks     Basic-block array for @p f (from @c regalloc_build_cfg()).
 * @param nBlocks    Number of entries in @p blocks.
 * @param nextVreg   Number of virtual registers in @p f (first physical reg id).
 * @param liveAfter  Per-instruction live sets from liveness analysis.
 * @param firstSpillVreg  Vreg id boundary marking reload temps: any node v
 *        with firstSpillVreg <= v < nextVreg is flagged isReloadTemp = 1.
 *        Pass a value <= 0 (or == nextVreg) to mark none.  The caller should
 *        snapshot f->nextVreg once before the first regalloc round and pass
 *        that same fixed value on every subsequent round, so temps
 *        introduced in round 1 stay correctly flagged in round 2, 3, etc.
 * @param arena      Arena for all IGraph fixed-size arrays.
 * @return           Fully initialised IGraph; call @c ig_free() when done.
 */
IGraph ig_build(const MachFunction *f, const BasicBlock *blocks, int nBlocks,
                RegClass cls, int classVregCount, const LiveSet *liveAfter,
                int firstSpillVreg, Arena *arena);

/**
 * @brief Free the heap-allocated adjacency-list data of every node.
 *
 * Only @c adj[i].data (owned by IntVector / malloc) is released here.
 * The fixed-size arrays (matrix, degree, color, etc.) are arena-allocated
 * and reclaimed when the caller destroys its arena — do not free them here.
 *
 * Always call this before @c arena_destroy() to avoid leaking the adjacency
 * list backing arrays, which live outside the arena.
 *
 * @param g  IGraph whose adjacency lists are to be released.
 */
void ig_free(IGraph *g);

#endif /* INTERFERENCE_H */