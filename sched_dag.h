/**
 * @file sched_dag.h
 * @brief Dependency DAG data structures and construction for instruction scheduling.
 *
 * Provides the SparseMap utility, the DAG node types (DAGNode, SuccNode), and
 * the build_dag() entry point consumed by the list scheduler in sched.c.
 *
 * ### Dependency kinds modelled
 * Every node in the DAG represents one machine instruction; directed edges
 * encode ordering constraints that every valid schedule must respect:
 *
 *  - **RAW** (Read-After-Write): instruction j reads a register last written
 *    by i → edge i→j.  The only true dependency; cannot be eliminated.
 *  - **WAW** (Write-After-Write): both i and j define the same register and
 *    j follows i in program order → edge i→j.  Preserved for physical
 *    registers; eliminated for virtual registers via renaming.
 *  - **WAR** (Write-After-Read): j overwrites a register still needed by i.
 *    Fully eliminated for virtual registers by assigning each definition a
 *    fresh rename id, so no WAR edges between vregs are ever emitted.
 *  - **Side-effect ordering**: STORE, CALL, IDIV, CQO are chained in program
 *    order through a lastSideEffect pointer regardless of register operands.
 *
 * ### Register renaming
 * Virtual registers receive a fresh "rename id" on every definition.  A
 * SparseMap (rename id → last defining instruction index) tracks the most
 * recent writer.  Reading r resolves currentName[r] to find the RAW source;
 * writing r allocates nextFresh++, records a WAW edge to the old writer, and
 * updates the map.  This eliminates WAR edges between vregs and reduces
 * spurious WAW edges to the truly unavoidable ones.
 *
 * ### Macro-fusion pinning
 * Intel decoders can fuse CMP/TEST + Jcc into one micro-op when they are
 * adjacent.  build_dag() sets pinnedForFusion = 1 on any CMP/TEST immediately
 * followed by a Jcc.  The scheduler (sched.c) never places pinned nodes in
 * the ready heap, leaving them to be emitted in program order at the end —
 * which guarantees the required adjacency.
 *
 * ### Critical-path height
 * Each node carries a height field: latency + max(height of successors),
 * computed in a single backward pass after all edges are built.  The list
 * scheduler uses height as the max-heap priority key so that the longest
 * dependency chains are started as early as possible, minimising stall cycles.
 *
 * ### Memory management
 * All allocations performed inside build_dag() (SparseMap arrays, currentName
 * table, SuccNode list nodes) come from the caller-supplied Arena.  The caller
 * also arena-allocates the DAGNode array itself.  sched.c resets the arena
 * between basic blocks, reclaiming all scratch memory without repeated
 * create/destroy overhead.  The arena must remain live for as long as any
 * DAGNode.succs pointer is dereferenced.
 */

#ifndef SCHED_DAG_H
#define SCHED_DAG_H

#include "instr_selector.h"
#include "arena.h"

/* =========================================================================
 * SparseMap — O(1) integer key → integer value mapping
 * =========================================================================
 * Classic sparse-set trick (EaC §B) extended with a value array.
 *
 * Invariant: dense[sparse[k]] == k  iff  k is currently in the map.
 * Membership is validated by the round-trip dense[sparse[k]] == k together
 * with the bounds check sparse[k] < n, so sparse[] does not need to be
 * zero-initialised — stale values from a previous block are harmless after
 * arena_reset() resets n to 0.
 *
 * Used in build_dag() to map (renamed register id) → (last defining
 * instruction index).  Key universe = nextVreg + PHYS_ALLOCATABLE + n,
 * where n extra slots accommodate the fresh rename ids allocated per block.
 * ========================================================================= */

/**
 * @brief Sparse integer-to-integer map with O(1) get and set.
 *
 * All three backing arrays are allocated from an external Arena; the struct
 * holds only pointers and metadata.  sparse[k] stores the candidate position
 * of key k in the dense[] array; the entry is valid only when
 * dense[sparse[k]] == k and sparse[k] < n.  dense[] and val[] are parallel:
 * val[i] is the value associated with the key dense[i].
 */
typedef struct {
    int *sparse; /**< sparse[k]: candidate position of key k in dense[].    */
    int *dense;  /**< dense[i]: the i-th active key.                        */
    int *val;    /**< val[i]: value associated with dense[i].               */
    int  n;      /**< Number of active key-value pairs currently stored.    */
    int  cap;    /**< Key universe size; all keys must satisfy 0 <= k < cap.*/
} SparseMap;

/**
 * @brief Initialise a SparseMap backed by @p arena with key universe [0, cap).
 *
 * Allocates the three backing arrays from @p arena.  The map starts empty
 * (n == 0); sparse[] is intentionally left uninitialised because the
 * round-trip validation makes stale values harmless.
 *
 * @param m     SparseMap to initialise.
 * @param cap   Key universe size; all keys must satisfy 0 <= k < cap.
 * @param arena Arena from which the three backing arrays are allocated.
 */
void smap_init(SparseMap *m, int cap, Arena *arena);

/**
 * @brief Look up the value for key @p k in O(1).
 *
 * Casts k to unsigned before the bounds check so negative keys are rejected
 * by a single comparison, avoiding a separate k < 0 test.  Validates via the
 * dense[sparse[k]] == k round-trip to distinguish live entries from stale ones.
 *
 * @param m  SparseMap to query (read-only).
 * @param k  Key to look up.
 * @return   Associated value, or -1 if k is absent or out of range.
 */
int  smap_get(const SparseMap *m, int k);

/**
 * @brief Insert or update the value for key @p k in O(1).
 *
 * If k is already present its value is updated in-place without disturbing
 * the dense/val layout.  Otherwise a new entry is appended and sparse[k] is
 * set to its position.  No-op if k is out of range.
 *
 * @param m  SparseMap to update.
 * @param k  Key to insert or update.
 * @param v  Value to associate with k.
 */
void smap_set(SparseMap *m, int k, int v);

/* =========================================================================
 * DAG node types
 * ========================================================================= */

/**
 * @brief A single outgoing dependency edge in a DAGNode's successor list.
 *
 * Allocated from the block's Arena (singly linked, prepended for O(1)
 * insertion).  The to field is a 0-based index into the local DAGNode array
 * (range [0, n) where n = end - start).
 */
typedef struct SuccNode {
    int              to;   /**< DAGNode index of the dependent successor.    */
    struct SuccNode *next; /**< Next edge in the singly-linked successor list.*/
} SuccNode;

/**
 * @brief A node in the instruction dependency DAG for one basic block.
 *
 * Populated by build_dag() in four sequential passes:
 *
 *  - **Pass 1** (init): instrIdx, latency, height set; all pointer/counter
 *    fields zeroed via compound-literal assignment.
 *  - **Pass 2** (macro-fusion scan): pinnedForFusion set where applicable.
 *  - **Pass 3** (forward edge building): succs, nSuccs, predCount updated as
 *    edges are added for RAW, WAW, and side-effect dependencies.
 *  - **Pass 4** (backward height): height recomputed as latency +
 *    max(successor heights); sink nodes retain height == latency.
 *
 * During scheduling (sched.c): scheduled is set to 1 when the node is
 * committed to the result; predCount is decremented as each predecessor
 * is committed, eventually making the node ready for the heap.
 */
typedef struct DAGNode {
    int       instrIdx;        /**< Index into f->instrs[] of the instruction. */
    int       latency;         /**< Estimated execution latency in cycles.      */
    int       height;          /**< Latency-weighted critical-path length to
                                *   any sink; used as max-heap priority key.    */
    int       predCount;       /**< Unscheduled predecessors; decremented by
                                *   the list scheduler as predecessors commit.  */
    int       scheduled;       /**< 1 once the node is placed in the output.   */
    int       pinnedForFusion; /**< 1 if this CMP/TEST must immediately precede
                                *   its Jcc (macro-fusion); excluded from heap. */
    SuccNode *succs;           /**< Head of the singly-linked successor list.  */
    int       nSuccs;          /**< Total number of outgoing dependency edges.  */
} DAGNode;

/* =========================================================================
 * DAG Construction
 * =========================================================================
 * build_dag() constructs the dependency DAG for the basic block spanning
 * f->instrs[start .. end-1].  The caller provides a pre-allocated DAGNode
 * array of size (end - start); all internal allocations (SparseMap arrays,
 * currentName renaming table, SuccNode objects) come from @p arena.
 *
 * The arena must outlive any use of nodes[] because DAGNode.succs pointers
 * reference SuccNode objects allocated inside it.  sched.c resets the arena
 * between blocks so all scratch memory is reclaimed automatically.
 * ========================================================================= */

/**
 * @brief Build the dependency DAG for the basic block f->instrs[start..end-1].
 *
 * Executes four sequential passes:
 *  1. Node initialisation: instrIdx, latency, height; all other fields zeroed.
 *  2. Macro-fusion scan: mark CMP/TEST + Jcc pairs with pinnedForFusion = 1.
 *  3. Forward edge-building with register renaming:
 *       - RAW edges via currentName[] + SparseMap lookup per source register.
 *       - Side-effect serialisation via a lastSideEffect chain.
 *       - WAW edge from previous writer + fresh rename id per definition.
 *  4. Backward height: height = latency + max(height of successors).
 *
 * @pre  nodes[] is an array of at least (end - start) DAGNode elements,
 *       accessible for the lifetime of @p arena.
 * @post nodes[i] is fully initialised for i in [0, end - start).  All
 *       SuccNode objects for edges are allocated inside @p arena.
 *
 * @param f      Machine function whose instruction stream is being analysed.
 * @param start  Index of the first instruction in the block (inclusive).
 * @param end    Index one past the last instruction in the block (exclusive).
 * @param nodes  Caller-allocated array of (end - start) DAGNode structs.
 * @param arena  Arena for SparseMap arrays, currentName table, and SuccNode
 *               objects; must remain live while nodes[] is in use.
 */
void build_dag(const MachFunction *f, int start, int end, DAGNode *nodes,
               Arena *arena);

#endif /* SCHED_DAG_H */