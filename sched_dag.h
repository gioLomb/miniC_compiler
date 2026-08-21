/**
 * @file sched_dag.h
 * @brief Dependency DAG data structures and construction for instruction scheduling.
 *
 * Provides the SparseMap utility, the DAG node types (DAGNode, SuccNode), and
 * the build_dag() entry point consumed by the list scheduler in sched.c.
 *
 * Dependency kinds modelled
 * -------------------------
 * Every node in the DAG represents one machine instruction; directed edges
 * encode ordering constraints every valid schedule must respect:
 *
 *  - **RAW** (Read-After-Write): j reads a register last written by i → edge i→j.
 *    The only true data dependency; cannot be eliminated.
 *  - **WAW** (Write-After-Write): both i and j define the same register and
 *    j follows i in program order → edge i→j.  Preserved for physical
 *    registers; reduced to the truly unavoidable case for virtual registers
 *    by the register-renaming step.
 *  - **WAR** (Write-After-Read): j overwrites a register still needed by i.
 *    Fully eliminated for virtual registers — each definition receives a
 *    fresh rename id, so no WAR edge between two vreg defs is ever emitted.
 *  - **Side-effect ordering**: STORE, CALL, IDIV, CQO are chained in program
 *    order through a lastSideEffect pointer, independent of register operands.
 *  - **Memory ordering**: LOAD and STORE instructions are serialised through
 *    a separate lastMemoryOp pointer to preserve load/store ordering without
 *    alias analysis.
 *
 * Register renaming
 * -----------------
 * Virtual registers receive a fresh "rename id" on every definition.  A
 * SparseMap (rename id → last defining instruction index) tracks the most
 * recent writer for each id.  Reading register r resolves currentName[r] to
 * find the RAW predecessor; writing r allocates nextFresh++, records a WAW
 * edge to the old writer (if any), and updates the map.  This eliminates WAR
 * edges between vregs and reduces WAW edges to truly unavoidable ones.
 *
 * Macro-fusion pinning
 * --------------------
 * Intel decoders can fuse CMP/TEST + Jcc into one micro-op when they are
 * adjacent.  build_dag() sets pinnedForFusion=1 on any CMP/TEST immediately
 * followed by a Jcc.  The scheduler never places pinned nodes in the ready
 * heap, leaving them to be emitted in program order at the end — guaranteeing
 * the required adjacency.
 *
 * Critical-path height
 * --------------------
 * After all edges are built, a single backward pass computes each node's
 * height: latency + max(height of all successors).  Sink nodes retain
 * height == latency.  The list scheduler uses height as the max-heap
 * priority key so that the longest dependency chains start as early as
 * possible, minimising pipeline stall cycles.
 *
 * Memory management
 * -----------------
 * All allocations performed inside build_dag() (SparseMap arrays, currentName
 * table, SuccNode list nodes) come from the caller-supplied Arena.  The
 * caller also arena-allocates the DAGNode array itself.  sched.c resets the
 * arena between basic blocks, reclaiming all scratch memory without repeated
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
 * Used by build_dag() to map (renamed register id) → (last defining
 * instruction index within the current block).
 *
 * Key universe = nextVreg + PHYS_ALLOCATABLE + n, where the extra n slots
 * accommodate the fresh rename ids allocated during renaming (one per
 * definition, up to n = block size).
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
    int *sparse; /**< sparse[k]: candidate position of key k in dense[].     */
    int *dense;  /**< dense[i]: the i-th active key (parallel with val[]).    */
    int *val;    /**< val[i]: value associated with the active key dense[i].  */
    int  n;      /**< Number of active key-value pairs currently stored.      */
    int  cap;    /**< Key universe size; all keys must satisfy 0 ≤ k < cap.  */
} SparseMap;

/**
 * @brief Initialise a SparseMap backed by @p arena with key universe [0, cap).
 *
 * Allocates the three backing arrays from @p arena.  The map starts empty
 * (n == 0); sparse[] is intentionally left uninitialised because the
 * round-trip validation makes stale values from a previous block harmless
 * after arena_reset() resets n to 0.
 *
 * @param m     SparseMap to initialise.
 * @param cap   Key universe size; all keys must satisfy 0 ≤ k < cap.
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
 * @param k  Key to look up; must be in [0, cap) for a valid result.
 * @return   Associated value if k is present, or -1 if absent/out-of-range.
 */
int  smap_get(const SparseMap *m, int k);

/**
 * @brief Insert or update the value for key @p k in O(1).
 *
 * If @p k is already present its value is updated in-place without
 * disturbing the dense/val layout.  If absent, a new entry is appended and
 * sparse[k] is set to its position.  No-op if k is out of range.
 *
 * @param m  SparseMap to update.
 * @param k  Key to insert or update; no-op if k < 0 or k ≥ cap.
 * @param v  Value to associate with @p k.
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
 * (range [0, n) where n = end - start for the current block).
 */
typedef struct SuccNode {
    int              to;   /**< DAGNode index of the dependent successor.     */
    struct SuccNode *next; /**< Next edge in the singly-linked successor list. */
} SuccNode;

/**
 * @brief A node in the instruction dependency DAG for one basic block.
 *
 * Populated by build_dag() in four sequential passes:
 *
 *  - **Pass 1** (init): instrIdx and latency set; height initialised to
 *    latency; all other fields zeroed via compound-literal assignment.
 *  - **Pass 2** (macro-fusion scan): pinnedForFusion set to 1 for any
 *    CMP/TEST immediately followed by a Jcc.
 *  - **Pass 3** (forward edge building): succs, nSuccs, and predCount
 *    updated as RAW, WAW, side-effect, and memory-ordering edges are added.
 *  - **Pass 4** (backward height propagation): height recomputed as
 *    latency + max(height of all successors); sink nodes retain the initial
 *    latency value.
 *
 * During scheduling (sched.c): scheduled is set to 1 when the node is
 * committed to the output; predCount is decremented as each predecessor
 * is scheduled, eventually making the node eligible for the ready heap.
 */
typedef struct DAGNode {
    int       instrIdx;        /**< Index into f->instrs[] of the instruction.  */
    int       latency;         /**< Estimated execution latency in cycles.       */
    int       height;          /**< Latency-weighted critical-path length to any
                                *   sink; used as the max-heap priority key.     */
    int       predCount;       /**< Unscheduled predecessors; decremented by the
                                *   list scheduler as predecessors are committed. */
    int       scheduled;       /**< 1 once this node has been placed in output.  */
    int       pinnedForFusion; /**< 1 if this CMP/TEST must immediately precede
                                *   its Jcc for macro-fusion; excluded from heap. */
    SuccNode *succs;           /**< Head of the singly-linked successor list.    */
    int       nSuccs;          /**< Total number of outgoing dependency edges.   */
} DAGNode;

/* =========================================================================
 * DAG construction
 * ========================================================================= */

/**
 * @brief Build the instruction dependency DAG for the block f->instrs[start..end-1].
 *
 * Executes four sequential passes over the instruction window:
 *
 *  1. **Node initialisation** — for each instruction i in [start, end):
 *     sets instrIdx = start+i, latency and height from sched_latency_of(),
 *     zeroes all pointer and counter fields.
 *
 *  2. **Macro-fusion scan** — scans adjacent pairs; sets pinnedForFusion=1
 *     on any CMP/TEST whose immediate successor is a Jcc.
 *
 *  3. **Forward edge-building with register renaming** — for each instruction
 *     j (in order):
 *       - Extracts use registers via sched_uses(); for each use r, resolves
 *         the current rename id currentName[r] and looks it up in the
 *         SparseMap to find the last writer dep; emits RAW edge dep→j.
 *       - If j has side effects, chains it after lastSideEffect.
 *       - If j is a LOAD or STORE, chains it after lastMemoryOp to
 *         serialise memory operations without alias analysis.
 *       - Extracts the defined register d via sched_def(); records a WAW
 *         edge from the previous writer of d's current rename id (if any);
 *         allocates a fresh rename id nextFresh++ and updates currentName[d]
 *         and the SparseMap.
 *
 *  4. **Backward height propagation** — scans nodes from n-1 down to 0;
 *     each node's height = latency + max(height of successors).
 *
 * @pre  nodes[] is an arena-allocated array of at least (end - start) DAGNode
 *       elements accessible for the lifetime of @p arena.
 * @post nodes[i] is fully initialised for i in [0, end - start).  All
 *       SuccNode objects for edges are allocated inside @p arena.
 *
 * @param f      Machine function whose instruction stream is analysed.
 * @param start  Index of the first instruction in the block (inclusive).
 * @param end    Index one past the last instruction in the block (exclusive).
 * @param nodes  Caller-allocated array of (end - start) DAGNode structs.
 * @param arena  Arena for SparseMap arrays, currentName table, and SuccNode
 *               objects; must remain live while nodes[] is in use.
 */
void build_dag(const MachFunction *f, int start, int end, DAGNode *nodes,
               Arena *arena);

#endif /* SCHED_DAG_H */