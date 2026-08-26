/**
 * @file sched_dag.h
 * @brief Dependency DAG data structures and construction for instruction scheduling.
 *
 * One DAGNode per machine instruction in a basic block. Directed edges encode
 * every ordering constraint a valid schedule must respect:
 *
 *  - RAW (Read-After-Write): true data dependency, never eliminated.
 *  - WAR (Write-After-Read):  eliminated for virtual registers via renaming
 *    (a fresh id per definition means no def can ever "overtake" an earlier
 *    read of a DIFFERENT generation) — the WAR edge only remains as a chain
 *    from the last reader of the CURRENT generation to the next writer.
 *  - WAW (Write-After-Write): reduced to the unavoidable case for vregs by
 *    the same renaming; kept in full for physical registers.
 *  - Side-effect ordering: STORE/PUSH/CALL/IDIV/CQO chained in program order.
 *  - Memory ordering: LOAD/STORE chained (no alias analysis).
 *
 * Physical registers are NOT renamed: instr_implicit_uses()/instr_implicit_defs()
 * (regalloc_utils.h) identify them by the fixed id (nextVreg + physReg), so
 * renaming would break that lookup. Only virtual registers get fresh ids.
 *
 * Macro-fusion pinning: a CMP/TEST immediately followed by a Jcc gets
 * pinnedForFusion=1, keeping the scheduler from separating them (Intel
 * decoder fuses adjacent CMP/TEST+Jcc into one micro-op).
 *
 * Height (critical-path length to any sink) is computed backward after all
 * edges exist; the list scheduler (sched.c) uses it as its priority key.
 *
 * Memory: every allocation build_dag() performs comes from the caller's
 * Arena, reset between blocks by sched.c — no per-block free needed.
 */

#ifndef SCHED_DAG_H
#define SCHED_DAG_H

#include "instr_selector.h"
#include "arena.h"

/** Upper bound on ids returned by one call to sched_uses / instr_implicit_*. */
#define SCHED_MAX_REG_IDS 16

/** @brief One outgoing dependency edge (singly-linked, arena-allocated). */
typedef struct SuccNode {
    int              to;   /**< DAGNode index of the dependent successor.      */
    struct SuccNode *next; /**< Next edge in this node's successor list.       */
} SuccNode;

/**
 * @brief One node in the instruction dependency DAG for a single basic block.
 */
typedef struct DAGNode {
    int       instrIdx;        /**< Index into f->instrs[] of the instruction.  */
    int       latency;         /**< Estimated execution latency in cycles.       */
    int       height;          /**< latency + max(height of successors); heap key. */
    int       predCount;       /**< Unscheduled predecessors (decremented by sched.c). */
    int       scheduled;       /**< 1 once committed to the scheduled output.    */
    int       pinnedForFusion; /**< 1 if this CMP/TEST must stay glued to its Jcc. */
    SuccNode *succs;           /**< Head of the successor edge list.             */
    int       nSuccs;          /**< Total outgoing edges.                        */
} DAGNode;

/**
 * @brief Build the instruction dependency DAG for f->instrs[start..end-1].
 *
 * @pre  nodes[] has at least (end - start) entries, arena-backed for the
 *       lifetime of @p arena.
 * @post nodes[i] fully populated for i in [0, end-start).
 *
 * @param f      Machine function whose instruction stream is analysed.
 * @param start  First instruction index of the block (inclusive).
 * @param end    One past the last instruction index of the block (exclusive).
 * @param nodes  Caller-allocated array of (end - start) DAGNode.
 * @param arena  Scratch arena for internal tracking arrays and SuccNode edges.
 */
void build_dag(const MachFunction *f, BlockRange blk, DAGNode *nodes,
               Arena *arena);

#endif /* SCHED_DAG_H */