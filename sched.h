#ifndef SCHED_H
#define SCHED_H


/**
 * @file sched.h
 * @brief Instruction Scheduler interface — Local List Scheduling per basic block.
 */

#include "instr_selector.h"

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