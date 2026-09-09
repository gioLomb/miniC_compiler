#ifndef RA_SPILL_H
#define RA_SPILL_H


/**
 * @file ra_spill.h
 * @brief Spill code insertion: rewrites spilled virtual registers as stack loads/stores.
 */

#include "instr_selector.h"

/* Worst-case per-instruction expansion, counted exactly for a single
 * original instruction:
 */
#define SPILL_MAX_EXPANSION_PER_INSTR 7
#define SPILL_EXTRA_MARGIN            4
#define BYTES_PER_QUADWORD 8

/**
 * @brief Insert load/store spill code for the given spilled virtual registers.
 *
 * Rewrites @p f->instrs in place (internal realloc): allocates one stack
 * slot per entry in @p spilled, replaces uses with cached reload
 * temporaries, and replaces defs with a store immediately after the
 * (possibly reload-modified) instruction.
 *
 * @param f        Machine function to rewrite (modified in place).
 * @param spilled  Array of vreg ids that must be spilled to the stack.
 * @param nSpilled Number of entries in @p spilled.
 * @param frameOff In/out running stack-frame offset; incremented by 8 for
 *                 each spilled vreg and left pointing past the last
 *                 allocated slot on return.
 */
void ra_spill_insert(MachFunction *f, const int *spilled, int nSpilled,
                     int *frameOff);

#endif /* RA_SPILL_H */