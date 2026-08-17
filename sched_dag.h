/**
 * @file sched_dag.h
 * @brief Dependency DAG data structures and construction for instruction scheduling.
 *
 * Overview
 * --------
 * This module provides the dependency Directed Acyclic Graph (DAG) used by the
 * local list scheduler (sched.c).  Every node in the DAG corresponds to one
 * machine instruction inside a basic block; directed edges encode ordering
 * constraints that must be respected by any valid schedule:
 *
 *   RAW (Read-After-Write): instruction j reads a register last written by
 *       instruction i → edge i→j.  Violating this would cause j to read a
 *       stale value.
 *
 *   WAW (Write-After-Write): both i and j define the same register, and j
 *       comes after i in the original order → edge i→j.  Without the edge,
 *       the scheduler could place j before i, making i's write invisible.
 *
 *   WAR (Write-After-Read): j overwrites a register that i still needs to
 *       read → edge i→j.  Only relevant for physical registers; for virtual
 *       registers the register-renaming pass in build_dag eliminates these
 *       false dependencies by assigning a fresh name to each definition,
 *       so WAR edges between vregs are never emitted.
 *
 *   Side-effect serialisation: memory stores, CALL, IDIV, CQO may have
 *       unmodelled effects; they are chained in program order so the
 *       scheduler cannot reorder them relative to one another.
 *
 * Critical-path height
 * ---------------------
 * Each node carries a @c height field — the length (in latency cycles) of
 * the longest path from that node to any sink (leaf) in the DAG.  The height
 * is computed in a single backward pass after all edges have been added.
 * The list scheduler (sched.c) uses height as the priority key of its
 * max-heap ready list: scheduling the node with the greatest height first
 * minimises the expected makespan by ensuring that high-latency dependency
 * chains are started as early as possible.
 *
 * Register renaming for false-dependency elimination
 * ---------------------------------------------------
 * Virtual registers are assigned a fresh "rename" id on every definition.
 * A SparseMap (rename id → last defining instruction index) tracks the
 * most recent writer of each renamed name.  When instruction j reads
 * register r, it looks up currentName[r] to find r's current rename and
 * resolves the RAW dependency.  When j writes r, a new rename is allocated,
 * the old rename's WAW edge is added if necessary, and the map is updated to
 * point to j.  This prevents spurious WAR/WAW edges between virtual registers
 * that would unnecessarily constrain the scheduler.
 *
 * Macro-fusion pinning
 * ---------------------
 * Intel decoders can fuse a CMP/TEST instruction immediately followed by a
 * Jcc into a single micro-op (macro-fusion), reducing front-end pressure.
 * build_dag() marks any CMP/TEST that is immediately followed by a Jcc with
 * @c pinnedForFusion = 1.  The scheduler (sched.c) never places such a node
 * in the ready heap, leaving it to be appended in its original program-order
 * position after the greedy pass — guaranteeing the CMP/TEST always
 * immediately precedes its Jcc in the final schedule.
 *
 * Memory management
 * -----------------
 * All dynamic allocations performed by build_dag() (SparseMap arrays,
 * SuccNode list nodes, the currentName renaming table) are made from the
 * caller-supplied Arena.  The DAGNode array itself is also allocated by the
 * caller before invoking build_dag().  This design means:
 *   - No per-node or per-edge malloc/free; the arena bulk-frees everything.
 *   - The arena must remain live as long as any DAGNode's @c succs pointer
 *     is dereferenced (SuccNode objects live inside the arena).
 *   - sched.c resets the arena between basic blocks so scratch memory is
 *     reused without repeated create/destroy overhead.
 *
 * Pipeline position
 * -----------------
 *   isel_select() → sched_schedule() [calls build_dag()] → regalloc()
 */

#ifndef SCHED_DAG_H
#define SCHED_DAG_H

#include "instr_selector.h"
#include "arena.h"

/* =========================================================================
 * SparseMap — O(1) integer key → integer value mapping
 * =========================================================================
 * Implements the classic sparse-set trick (EaC §B) extended with a value
 * array, giving O(1) get, O(1) set, and O(n) clear (via arena_reset).
 *
 * Invariant: dense[sparse[k]] == k  iff  k is currently in the map.
 * Membership is validated by checking the sparse index against both the
 * active-count bound (n) and the round-trip through dense[], which
 * correctly rejects stale entries from a previous block whose arena was
 * reset rather than re-zeroed.
 *
 * Used in build_dag() to map "renamed register id → last defining
 * instruction index" in O(1) per lookup/update.  The key universe equals
 * nextVreg + PHYS_ALLOCATABLE + n (extra slots for renamed vregs); the map
 * is allocated fresh for every basic block from the block's arena.
 * ========================================================================= */

/**
 * @brief Sparse integer-to-integer map with O(1) get and set.
 *
 * All three arrays (sparse, dense, val) are allocated from an external
 * Arena; the struct itself only holds pointers and metadata.
 *
 * @c sparse[k] stores the position of key @c k in @c dense[]; the entry
 * is valid only if @c dense[sparse[k]] == k and @c sparse[k] < n.
 * @c dense[] and @c val[] are parallel arrays: @c val[i] is the value
 * associated with the key @c dense[i].
 */
typedef struct {
    int *sparse; /**< sparse[k]: candidate position of key k in dense[].    */
    int *dense;  /**< dense[i]: the i-th active key.                        */
    int *val;    /**< val[i]: value associated with dense[i].               */
    int  n;      /**< Number of active key-value pairs currently stored.    */
    int  cap;    /**< Key universe size; all keys must be in [0, cap).      */
} SparseMap;

/**
 * @brief Initialise a SparseMap backed by @p arena with key universe [0, cap).
 *
 * Allocates the three backing arrays from @p arena.  The map starts empty
 * (n == 0); @c sparse[] is intentionally left uninitialised because
 * membership queries validate against @c dense[], making stale entries
 * harmless.
 *
 * @param m     SparseMap to initialise.
 * @param cap   Key universe size; keys must satisfy 0 <= k < cap.
 * @param arena Arena from which the three arrays are allocated.
 */
void smap_init(SparseMap *m, int cap, Arena *arena);

/**
 * @brief Look up the value associated with key @p k in O(1).
 *
 * Validates via the dense[sparse[k]] == k round-trip to distinguish live
 * entries from stale ones that happen to have the same sparse index.
 *
 * @param m  SparseMap to query (const: read-only).
 * @param k  Key to look up.
 * @return   Associated value, or -1 if @p k is absent or out of range.
 */
int  smap_get(const SparseMap *m, int k);

/**
 * @brief Insert or update the value for key @p k in O(1).
 *
 * If @p k is already present its value is updated in place; otherwise a new
 * entry is appended to the dense/val arrays and sparse[k] is set to point
 * to the new position.  No-op if @p k is out of range.
 *
 * @param m  SparseMap to update.
 * @param k  Key to insert or update.
 * @param v  Value to associate with @p k.
 */
void smap_set(SparseMap *m, int k, int v);

/* =========================================================================
 * DAG node types
 * ========================================================================= */

/**
 * @brief A single outgoing dependency edge in a DAGNode's successor list.
 *
 * The list is singly linked and allocated from the block's Arena, so no
 * individual free is needed.  The @c to field is an index into the local
 * DAGNode array (range [0, n) where n = end - start).
 */
typedef struct SuccNode {
    int              to;   /**< DAGNode index of the successor.              */
    struct SuccNode *next; /**< Next edge in the singly-linked successor list.*/
} SuccNode;

/**
 * @brief A node in the instruction dependency DAG for one basic block.
 *
 * Each node represents one machine instruction.  Fields are populated by
 * build_dag() during three sequential passes:
 *
 *  Pass 1 (forward, edge building): @c instrIdx, @c latency are set at
 *    initialisation; @c succs, @c nSuccs, @c predCount are updated as edges
 *    are added; @c pinnedForFusion is set during the CMP/TEST+Jcc scan.
 *
 *  Pass 2 (backward, height computation): @c height is computed as
 *    latency + max(height of successors), giving the length of the longest
 *    latency-weighted path from this node to any sink.
 *
 *  Scheduling phase (sched.c): @c scheduled and @c predCount are mutated
 *    by the greedy list scheduler as nodes are committed to the result array.
 */
typedef struct DAGNode {
    int       instrIdx;       /**< Index into f->instrs[] of the represented instruction. */
    int       latency;        /**< Estimated execution latency in cycles (Agner Fog).     */
    int       height;         /**< Latency-weighted critical-path length to any sink.     */
    int       predCount;      /**< Number of unscheduled predecessors; decremented as
                                *   predecessors are committed by the list scheduler.     */
    int       scheduled;      /**< 1 once this node has been placed in the result array.  */
    int       pinnedForFusion;/**< 1 if this CMP/TEST must immediately precede its Jcc
                                *   (macro-fusion preservation); excluded from ready heap. */
    SuccNode *succs;          /**< Head of the singly-linked list of successor edges.     */
    int       nSuccs;         /**< Total number of outgoing edges (informational).        */
} DAGNode;

/* =========================================================================
 * DAG Construction
 * =========================================================================
 * build_dag() constructs the dependency DAG for the basic block spanning
 * instructions f->instrs[start .. end-1].  The caller provides a pre-
 * allocated DAGNode array of size (end - start); build_dag() populates it
 * and allocates all edge list nodes and auxiliary tables from @p arena.
 *
 * The arena must outlive any use of the returned DAGNode array because
 * DAGNode.succs pointers refer into arena-allocated SuccNode objects.
 *
 * Typical usage (from sched.c):
 *
 *   arena_reset(arena);                          // reclaim previous block
 *   DAGNode *nodes = arena_alloc(arena, n * sizeof(DAGNode));
 *   build_dag(f, start, end, nodes, arena);
 *   // ... schedule using nodes[] ...
 * ========================================================================= */

/**
 * @brief Build the dependency DAG for instructions f->instrs[start .. end-1].
 *
 * Performs four sequential sub-passes:
 *
 *  1. Node initialisation: set instrIdx, latency, height for each node;
 *     zero all other fields via compound-literal assignment.
 *
 *  2. Macro-fusion scan: mark CMP/TEST nodes immediately followed by Jcc
 *     with pinnedForFusion = 1 so the scheduler preserves their adjacency.
 *
 *  3. Forward edge-building pass:
 *       - For each instruction j, resolve RAW dependencies: look up each
 *         source register's current rename in the SparseMap and add an
 *         edge from the last writer to j.
 *       - Serialize side-effecting instructions (STORE, CALL, IDIV, CQO)
 *         by chaining them through @c lastSideEffect in program order.
 *       - For each definition, add a WAW edge from the previous writer of
 *         the same rename, allocate a fresh rename for the new write, and
 *         record j as the latest writer in the SparseMap.
 *
 *  4. Backward height computation: for each node in reverse order,
 *     height = latency + max(height of successors).  Nodes with no
 *     successors (sinks) have height == latency.
 *
 * @pre  nodes[] is an array of at least (end - start) DAGNode elements,
 *       accessible for the lifetime of @p arena.
 * @pre  @p arena is live and has enough capacity for SparseMap arrays,
 *       currentName array, and all SuccNode allocations.
 * @post nodes[i] is fully initialised for i in [0, end - start).
 *       SuccNode objects for all edges are allocated inside @p arena.
 *
 * @param f      Machine function whose instruction array is being analysed.
 * @param start  Index of the first instruction in the block (inclusive).
 * @param end    Index one past the last instruction in the block (exclusive).
 * @param nodes  Caller-allocated array of (end - start) DAGNode structs.
 * @param arena  Arena for SparseMap, currentName table, and SuccNode objects.
 */
void build_dag(const MachFunction *f, int start, int end, DAGNode *nodes,
               Arena *arena);

#endif /* SCHED_DAG_H */