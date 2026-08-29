/**
 * @file interference.h
 * @brief Interference graph (IGraph) construction for register allocation.
 *
 * An interference graph models which virtual registers cannot share the same
 * physical register because they are simultaneously live at some program
 * point.  Each node corresponds to one virtual register (id in [0, nextVreg));
 * an edge (u, v) means u and v interfere.
 *
 * Representation
 * --------------
 * Two complementary representations are maintained:
 *
 *  - Triangular bit matrix (@c matrix): provides O(1) edge existence queries
 *    and is compact in memory.  Entry for the pair (i, j) with i > j is at
 *    bit position i*(i-1)/2 + j within the flat uint64_t array.
 *
 *  - Adjacency lists (@c adj): one IntVector per node.  Used by Simplify
 *    (ra_color.c) to iterate over a node's neighbours when updating degrees
 *    after a node is removed from the graph.
 *
 * Physical-register nodes
 * -----------------------
 * Nodes in [nextVreg, nextVreg + PHYS_ALLOCATABLE) represent physical
 * registers.  They are pre-coloured (color[p] = p - nextVreg) and always
 * active.  Edges from virtual registers to physical registers are added when
 * a virtual register is live across an instruction that implicitly defines a
 * physical register (e.g. IDIV clobbers RAX/RDX, CALL clobbers all
 * caller-saved registers).
 *
 * Exclusion masks and call-crossing flags
 * ----------------------------------------
 * @c excl[v] is a bitmask of physical-register colours that @p v must not
 * receive (in addition to interference edges).  Used for partial clobbers
 * (SETcc writes %al = PHYS_RAX).
 * @c crossesCall[v] is 1 if @p v is live across at least one CALL; the
 * colour selector then prefers callee-saved registers for @p v to minimise
 * push/pop overhead in the prologue/epilogue.
 *
 * Reload-temp flag
 * -----------------
 * @c isReloadTemp[v] is 1 if @p v is a reload/spill temporary introduced by
 * ra_spill_insert() in a previous round of the same function's regalloc
 * loop (see ig_build()'s firstSpillVreg parameter).  Such temps are scoped
 * to 2-3 instructions by construction (one load-use or def-store), so they
 * naturally have a very low spillCost.  Left unguarded, the Briggs-optimistic
 * spill heuristic in ra_simplify() picks them as the "cheapest" candidate by
 * the spillCost/degree ratio, even though respilling them does nothing to
 * relieve real register pressure — that pressure comes from other, genuinely
 * long-lived values.  This produced observed pathological cases: a single
 * value bounced through 15+ stack slots across that many regalloc rounds
 * before the heuristic finally picked a real candidate.  ra_simplify() uses
 * this flag to prefer non-reload-temp candidates first.
 *
 * Lifetime
 * --------
 * Fixed-size arrays (matrix, degree, color, active, excl, spillCost,
 * crossesCall, isReloadTemp) are allocated from the caller-supplied arena.
 * Adjacency list data (adj[i].data) is heap-allocated by IntVector and must
 * be released via ig_free() before the arena is destroyed.
 */

#ifndef INTERFERENCE_H
#define INTERFERENCE_H

#include <stdbool.h>
#include "liveness.h"
#include "instr_selector.h"
#include "regalloc_utils.h"
#include "arena.h"
#include "dynamic_array.h"

#define IG_ADJ_INITIAL_CAPACITY 8

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
    int       n;        /**< Total node count: nextVreg + PHYS_ALLOCATABLE.
                         *   Nodes [0, nextVreg) are virtual; nodes
                         *   [nextVreg, n) are pre-coloured physical regs.   */
    uint64_t *matrix;   /**< Compact lower-triangular bit matrix.
                         *   Bit for pair (i,j) with i>j lives at position
                         *   i*(i-1)/2 + j inside the flat uint64_t array.  */
    AdjList  *adj;      /**< Per-node adjacency list (arena headers, heap data). */
    int      *degree;   /**< Current interference degree of each node.
                         *   Decremented as nodes are removed during Simplify. */
    int      *color;    /**< Assigned physical-register colour.
                         *   -1 = uncoloured (pre-allocation),
                         *   -2 = spilled (no colour available),
                         *   >=0 = index in [0, PHYS_ALLOCATABLE).           */
    bool     *active;   /**< 1 while the node is still in the graph (not
                         *   removed by Simplify or pre-assigned as physical). */
    uint32_t *excl;     /**< Bitmask of forbidden physical-register colours
                         *   beyond what interference edges already encode.
                         *   Set for partial clobbers (SETcc writes %al).    */
    int      *spillCost;    /**< Estimated cost of spilling this virtual register.
                             *   Accumulated as loop-depth-weighted def/use count. */
    char     *crossesCall;  /**< 1 if the vreg is live across at least one CALL.
                             *   Used by colour selector to prefer callee-saved regs,
                             *   reducing push/pop overhead in the function frame.  */
    char     *isReloadTemp; /**< 1 if this node is a reload/spill temp introduced
                             *   by a previous ra_spill_insert() round (see the
                             *   module-level doc above and ig_build()'s
                             *   firstSpillVreg parameter). Consulted by
                             *   ra_simplify() to avoid respilling short-lived
                             *   spill-code temporaries ahead of genuinely
                             *   long-lived values.                              */
} IGraph;

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
 * @param blocks     Basic-block array for @p f (from @c build_cfg()).
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
                int nextVreg, const LiveSet *liveAfter, int firstSpillVreg,
                Arena *arena);

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