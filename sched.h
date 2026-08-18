#ifndef SCHED_H
#define SCHED_H

#include "instr_selector.h"

/**
 * @file sched.h
 * @brief Instruction Scheduler interface — Local List Scheduling per basic block.
 *
 * Target: Intel x86-64 Out-of-Order architectures (Haswell / Broadwell).
 *
 * Objective:
 * Reorder instructions within individual basic blocks to:
 * 1. Hoist high-latency instructions (e.g., IMUL, IDIV, LOAD, STORE) early so
 * that the out-of-order execution pipeline finds operands ready ahead of time.
 * 2. Preserve adjacent CMP/TEST + Jcc instruction pairs to leverage Intel decoder
 * macro-fusion (merging two instructions into a single micro-op).
 * 3. Enforce all True (RAW), Anti (WAR), and Output (WAW) data dependencies for correctness.
 *
 * Algorithm:
 * Forward list scheduling using a priority queue ordered by node height on the
 * latency-weighted critical path toward a sink (longest path to basic block end).
 *
 * Internal Structure:
 * Uses an array of DAG node pointers (EaC §4.4.3) for O(1) random access during edge
 * construction and ready-list scanning.
 *
 * Known Limitations:
 * - Local scheduling scope (basic-block level only); ignores inter-block dataflow.
 * - Static latency model (Agner Fog Haswell tables); ignores cache misses, branch
 * prediction, and execution port throughput bottlenecks.
 * - Register renaming eliminates false WAR/WAW dependencies on virtual registers during
 * DAG build; physical register dependencies are treated as hard edges.
 */

/**
 * @brief Performs local list instruction scheduling across all functions in a program.
 *
 * Drives basic-block identification, DAG construction, and list scheduling for every
 * machine function contained in the program instance.
 *
 * @param mp Pointer to the target machine program.
 */
void sched_schedule(MachProgram *mp);

#endif /* SCHED_H */