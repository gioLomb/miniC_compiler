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
 * Lifetime
 * --------
 * Fixed-size arrays (matrix, degree, color, active, excl, spillCost,
 * crossesCall) are allocated from the caller-supplied arena.  Adjacency
 * list data (adj[i].data) is heap-allocated by IntVector and must be
 * released via ig_free() before the arena is destroyed.
 */

#ifndef INTERFERENCE_H
#define INTERFERENCE_H

#include <stdbool.h>
#include "liveness.h"
#include "instr_selector.h"
#include "regalloc_utils.h"
#include "arena.h"
#include "dynamic_array.h"

/** Maximum number of explicit def operands extracted per instruction. */
#define MAX_INSTR_OPERANDS 16
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
    int       n;        /**< Total number of nodes (nextVreg + PHYS_ALLOCATABLE). */
    uint64_t *matrix;   /**< Triangular bit matrix encoding edge existence.       */
    AdjList  *adj;      /**< Per-node adjacency list (arena-allocated headers).   */
    int      *degree;   /**< Current interference degree of each node.            */
    int      *color;    /**< Assigned colour (-1 = uncoloured, -2 = spilled).     */
    bool     *active;   /**< 1 if the node is still in the graph (not removed).   */
    uint32_t *excl;     /**< Bitmask of forbidden physical-register colours.      */
    int      *spillCost;    /**< Estimated cost of spilling this virtual register. */
    char     *crossesCall;  /**< 1 if the vreg is live across at least one CALL.  */
} IGraph;

/**
 * @brief Build the interference graph from the machine-code liveness result.
 *
 * For every instruction @c i and every register @c d that @c i defines:
 * an interference edge (d, v) is added for every register @c v live in
 * @c liveAfter[i].  Implicit defs (IDIV, CQO, CALL) are processed the same
 * way.  CALL instructions additionally set @c excl and @c crossesCall for
 * every live virtual register, to guide colour selection.
 *
 * Spill costs are accumulated during the same scan: each instruction
 * contributes @c regalloc_spill_weight(loopDepth) to the cost of every
 * vreg it explicitly uses or defines.
 *
 * @pre  @p liveAfter must be the per-instruction liveness array produced by
 *       @c liveness_computeMach() for the same function.
 * @pre  The caller-supplied @p arena must remain live as long as the IGraph
 *       is in use.
 *
 * @param f          Machine function to analyse.
 * @param blocks     Basic-block array for @p f (from @c build_cfg()).
 * @param nBlocks    Number of entries in @p blocks.
 * @param nextVreg   Number of virtual registers in @p f.
 * @param liveAfter  Per-instruction live sets from liveness analysis.
 * @param arena      Arena for all IGraph fixed-size arrays.
 * @return           Fully initialised IGraph; call @c ig_free() when done.
 */
IGraph ig_build(const MachFunction *f, const BasicBlock *blocks, int nBlocks,
                int nextVreg, const LiveSet *liveAfter, Arena *arena);

/**
 * @brief Free the heap-allocated adjacency-list data of every node.
 *
 * Releases only @c adj[i].data (owned by IntVector / malloc).  The fixed-
 * size arrays are arena-allocated and are freed when the caller destroys
 * its arena.
 *
 * @param g  IGraph whose adjacency lists are to be released.
 */
void ig_free(IGraph *g);

#endif /* INTERFERENCE_H */