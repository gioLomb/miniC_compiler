

#ifndef SCHED_DAG_H
#define SCHED_DAG_H


/**
 * @file sched_dag.h
 * @brief Dependency DAG data structures and construction for instruction scheduling.
 */

#include "instr_selector.h"
#include "arena.h"
#include "bitset.h"   

/** Upper bound on ids returned by one call to sched_uses / instr_implicit_*. */
#define SCHED_MAX_REG_IDS 16

/** @brief One outgoing dependency edge (singly-linked, arena-allocated). */
typedef struct SuccNode {
    int              to;   /**< DAGNode index of the dependent successor.      */
    struct SuccNode *next; /**< Next edge in this node's successor list.       */
} SuccNode;

typedef struct DAGNode {
    int       instrIdx;        /**< Index into f->instrs[] of the instruction.  */
    int       latency;         /**< Estimated execution latency in cycles.       */
    int       height;          /**< latency + max(height of successors); heap key. */
    int       predCount;       /**< Unscheduled predecessors (decremented by sched.c). */
    int       scheduled;       /**< 1 once committed to the scheduled output.    */
    int       pinnedForFusion; /**< 1 if this CMP/TEST must stay glued to its Jcc. */
    SuccNode *succs;           /**< Head of the successor edge list.             */
    int       nSuccs;          /**< Total outgoing edges.                        */
    BitSet    connectedTo;     /**< Bit i set iff an edge to local node i already
                                *   exists; O(1) dedup check in dag_add_edge()
                                *   instead of walking succs (O(degree) per call,
                                *   O(n^2) worst case when many instructions
                                *   share a def/use on the same register). */
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
void dag_build(const MachFunction *f, BlockRange blk, DAGNode *nodes,
               Arena *arena);

#endif /* SCHED_DAG_H */