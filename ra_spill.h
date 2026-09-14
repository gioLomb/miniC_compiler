#ifndef RA_SPILL_H
#define RA_SPILL_H

/**
 * @file ra_spill.h
 * @brief Spill code insertion for a single register class.
 */

#include "instr_selector.h"
#include "reg_class.h"

#define SPILL_MAX_EXPANSION_PER_INSTR 7
#define SPILL_EXTRA_MARGIN            4
#define BYTES_PER_QUADWORD            8

/**
 * @brief Insert load/store spill code for spilled vregs of class @p cls.
 *
 * Uses MACH_MOV / MO_VREG for RC_INT and MACH_MOVSS / MO_VREG_F for RC_FLOAT.
 * Grows the class's vreg counter (nextVreg or fNextVreg) for reload temps.
 */
void ra_spill_insert(MachFunction *f, RegClass cls, const int *spilled, int nSpilled,
                     int *frameOff);

#endif /* RA_SPILL_H */
