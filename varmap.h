#ifndef VARMAP_H
#define VARMAP_H


/**
 * @file varmap.h
 * @brief Dense operand → id map for IR analysis (liveness, CP, DCE, isel).
 *
 * Only OPND_VAR and OPND_TEMP are mapped. Ids are dense in [0, count) so
 * they can index bitsets. Temps are looked up by tempId; user variables by
 * (varLevel, varOffset). Implementation is two growable tables + nextId —
 * no hash table, no version/cache invalidation.
 */

#include "ir.h"

typedef struct VarMap VarMap;

/**
 * @brief Get or create the dense id for a (kind, level/tempId, offset) triple.
 *
 * @param kind  0 = OPND_VAR, 1 = OPND_TEMP.
 * @param a     varLevel or tempId.
 * @param b     varOffset (0 for temps).
 * @return      Id in [0, varmap_count(m)).
 */
int varmap_mapToIndex(VarMap *m, int kind, int a, int b);

/**
 * @brief Map an IR Operand to its dense id, or -1 if not storage.
 *
 * OPND_VAR / OPND_TEMP → get-or-create id; all other kinds → -1.
 */
int varmap_operand_id(VarMap *m, Operand op);

/** @brief Empty map; pair with varmap_destroy(). */
VarMap *varmap_create(void);

/** @brief Number of distinct ids assigned ([0, count)). */
int varmap_count(const VarMap *m);

/** @brief Free @p m and its tables. */
void varmap_destroy(VarMap *m);

#endif
