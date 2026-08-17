#ifndef SCHED_DAG_H
#define SCHED_DAG_H

#include "instr_selector.h"

/**
 * @file sched_dag.h
 * @brief Data structures and DAG construction routines for instruction scheduling.
 */

/* =========================================================================
 * SparseMap Interface
 * =========================================================================
 * Dense/Sparse map mapping integer keys -> integer values with O(1) operations.
 *
 * Classic sparse-set invariant (EaC §B):
 *   dense[sparse[k]] == k   iff   k is present in the set.
 *
 * Note: `sparse[]` is left uninitialized; membership testing validates against
 * `dense[]` bounds to eliminate false positives.
 * ========================================================================= */

/**
 * @brief Sparse set representation mapping integer keys to values in $O(1)$ time.
 */
typedef struct {
    int *sparse; /**< Sparse index table (`sparse[key]` -> position in `dense`). */
    int *dense;  /**< Dense array storing registered key identifiers.             */
    int *val;    /**< Value array corresponding to `dense[pos]`.                */
    int  n;      /**< Current number of active populated entries.                */
    int  cap;    /**< Total key universe capacity (`keys in [0, cap)`).         */
} SparseMap;

/**
 * @brief Initializes a SparseMap using internal module arena allocation.
 *
 * @param m   Pointer to the SparseMap structure.
 * @param cap Maximum key universe size.
 */
void smap_init(SparseMap *m, int cap);

/**
 * @brief Look up a key's associated value in $O(1)$ time.
 *
 * @param m Pointer to the SparseMap instance.
 * @param k Key identifier to query.
 * @return Associated integer value, or `-1` if key is not present.
 */
int smap_get(const SparseMap *m, int k);

/**
 * @brief Binds or updates a key-value pair in $O(1)$ time.
 *
 * @param m Pointer to the SparseMap instance.
 * @param k Key identifier.
 * @param v Value to associate with `k`.
 */
void smap_set(SparseMap *m, int k, int v);

/* =========================================================================
 * MaxHeap Priority Queue
 * ========================================================================= */

/**
 * @brief Binary Max-Heap priority queue ordered by latency-weighted path height.
 */
typedef struct {
    int *data; /**< Array of DAG node indices. */
    int  size; /**< Active element count.      */
} MaxHeap;

/**
 * @brief Edge list node representing a successor dependency in the DAG.
 */
typedef struct SuccNode {
    int              to;   /**< Index of the successor DAG node. */
    struct SuccNode *next; /**< Pointer to the next successor link.   */
} SuccNode;

/**
 * @brief Representation of an instruction node within the dependency DAG.
 */
typedef struct DAGNode {
    int       instrIdx;        /**< Index in the target function's instruction array.   */
    int       latency;         /**< Estimated execution latency of the instruction.     */
    int       height;          /**< Critical path height weighted by latency to a sink. */
    int       predCount;       /**< Count of unscheduled predecessor instructions.       */
    int       scheduled;       /**< Flag indicating if node has been scheduled.         */
    int       pinnedForFusion; /**< Macro-fusion flag (e.g., CMP/TEST pairing Jcc).      */
    SuccNode *succs;           /**< Head of the linked list of successor nodes.          */
    int       nSuccs;          /**< Total number of outgoing successor edges.            */
} DAGNode;

/**
 * @brief Pushes a node index into the MaxHeap based on critical path height.
 *
 * @param h     Pointer to the heap instance.
 * @param v     DAG node index to insert.
 * @param nodes Array of DAG nodes used for priority comparisons.
 */
void heap_push(MaxHeap *h, int v, const DAGNode *nodes);

/**
 * @brief Pops the highest priority DAG node index from the heap.
 *
 * @param h     Pointer to the heap instance.
 * @param nodes Array of DAG nodes used for priority comparisons.
 * @return Index of the highest priority DAG node.
 */
int heap_pop(MaxHeap *h, const DAGNode *nodes);

/* =========================================================================
 * Arena Lifecycle Management
 * ========================================================================= */

/**
 * @brief Allocates and initializes the internal module arena.
 * Must be called prior to invoking `build_dag()`.
 */
void sched_dag_arena_init(void);

/**
 * @brief Releases the internal module arena and associated resources.
 */
void sched_dag_arena_fini(void);

/* =========================================================================
 * DAG Construction
 * ========================================================================= */

/**
 * @brief Constructs the dependency DAG for instructions in the basic block range [start, end).
 *
 * Resets internal arena memory and builds data dependency edges (RAW, WAR, WAW)
 * alongside side-effect serialization links.
 *
 * @param f     Pointer to the enclosing machine function.
 * @param start Inclusive start index of the basic block.
 * @param end   Exclusive end index of the basic block.
 * @param nodes Pre-allocated output array of `DAGNode` structures allocated by caller.
 */
void build_dag(const MachFunction *f, int start, int end, DAGNode *nodes);

#endif /* SCHED_DAG_H */