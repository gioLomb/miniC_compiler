/**
 * @file constmap.h
 * @brief Constant-propagation lattice and per-variable constant map.
 *
 * This module provides two interrelated abstractions used exclusively by the
 * Constant Propagation pass (cp.c):
 *
 *  1. LatVal — a single element of the constant-propagation lattice.
 *  2. ConstMap — a dense array of LatVal, one entry per tracked variable id,
 *     indexed by the compact integer ids produced by VarMap.
 *
 * The lattice
 * -----------
 * The lattice is ordered UNKNOWN < CONST(v) < CONFLICT and has three levels:
 *
 *   UNKNOWN   (⊤)  No definition of this variable has been seen yet on any
 *                   reaching path.  Acts as the identity element for meet:
 *                   UNKNOWN ⊓ x = x.
 *
 *   CONST(v)       The variable holds the same compile-time constant value v
 *                   on every path reaching this point.  Both integer and
 *                   float constants are represented.
 *
 *   CONFLICT  (⊥)  The variable's value differs across reaching paths, or
 *                   comes from a non-constant definition (e.g. a load or call).
 *                   Absorbs everything: CONFLICT ⊓ x = CONFLICT.
 *
 * Meet operation (⊓)
 * ------------------
 * Used at CFG join points to combine information from all predecessors:
 *
 *   UNKNOWN  ⊓  x         = x
 *   CONST(a) ⊓  CONST(a)  = CONST(a)   (same value: still a constant)
 *   CONST(a) ⊓  CONST(b)  = CONFLICT   (a ≠ b: cannot determine statically)
 *   CONFLICT ⊓  x         = CONFLICT
 *
 * Transfer function helpers
 * -------------------------
 * The bottom half of this header declares the helpers called by cp.c's
 * transferInstr() to evaluate whether an instruction produces a constant
 * result and, if so, what value:
 *
 *   lat_get_value_from_operand — lift an Operand to a LatVal (inline constants or map lookup)
 *   is_binary_op      — predicate: opcode takes two operands and produces a value
 *   fold_binary_int   — constant-fold a binary integer operation
 *   fold_binary_float — constant-fold a binary float operation
 *   lat_fold_unary       — constant-fold NEG or NOT
 */

#ifndef CONSTMAP_H
#define CONSTMAP_H

#include "ir.h"
#include "varmap.h"
#include "arena.h"

/* =========================================================================
 * Lattice level constants
 * ========================================================================= */

#define LAT_UNKNOWN   0   /**< No information yet (⊤, identity for meet). */
#define LAT_CONST     1   /**< Variable holds a single known constant value. */
#define LAT_CONFLICT  2   /**< Variable's value is non-constant or ambiguous (⊥). */

/* =========================================================================
 * LatVal — one lattice element
 * ========================================================================= */

/**
 * @brief A single element of the constant-propagation lattice.
 *
 * When @c state is @c LAT_CONST, the @c val union holds the constant value
 * and @c isFloat distinguishes between integer and floating-point constants.
 * For @c LAT_UNKNOWN and @c LAT_CONFLICT the @c val and @c isFloat fields
 * carry no meaningful information.
 */
typedef struct {
    int state;       /**< One of LAT_UNKNOWN, LAT_CONST, LAT_CONFLICT. */
    int isFloat;     /**< 1 when the constant is a float, 0 for integer. */
    union {
        int   ival;  /**< Integer constant value (when isFloat == 0). */
        float fval;  /**< Float constant value   (when isFloat == 1). */
    } val;
} LatVal;

/* =========================================================================
 * Lattice constructors
 * ========================================================================= */


/** @brief Return a CONST lattice element wrapping the integer @p ival. */
LatVal lat_set_const_int(int ival);

/** @brief Return a CONST lattice element wrapping the float @p fval. */
LatVal lat_set_const_float(float fval);

/** @brief Return the CONFLICT (⊥) lattice element. */
LatVal lat_conflict(void);

/* =========================================================================
 * Lattice operations
 * ========================================================================= */

/**
 * @brief Compute the meet (⊓) of two lattice elements.
 *
 * Implements the confluence operation for join points in the dataflow graph.
 * UNKNOWN is the identity, CONFLICT is absorbing, and two equal CONST values
 * meet to themselves; any other pair of CONST values meets to CONFLICT.
 *
 * @param a  First lattice element.
 * @param b  Second lattice element.
 * @return   The greatest lower bound of @p a and @p b in the lattice ordering.
 */
LatVal lat_meet(LatVal a, LatVal b);

/**
 * @brief Return non-zero if @p a and @p b represent the same lattice element.
 *
 * Two CONST elements are equal only when they share the same type (int/float)
 * and the same value; all UNKNOWN elements are equal to each other, as are
 * all CONFLICT elements.
 *
 * @param a  First lattice element.
 * @param b  Second lattice element.
 * @return   1 if equal, 0 otherwise.
 */
int lat_equal(LatVal a, LatVal b);

/** @brief Return non-zero if @p v is a CONST lattice element. */
int lat_is_const(LatVal v);

/** @brief Return non-zero if @p v is the UNKNOWN lattice element. */
int lat_is_unknown(LatVal v);

/** @brief Return non-zero if @p v is the CONFLICT lattice element. */
int lat_is_conflict(LatVal v);

/* =========================================================================
 * ConstMap — per-variable lattice state
 * ========================================================================= */

/**
 * @brief Dense array mapping compact variable ids to their current LatVal.
 *
 * The index space corresponds to the ids assigned by VarMap: if VarMap
 * assigns id @c k to operand @c op, then @c vals[k] holds the lattice
 * value for @c op at the current program point.  All storage is owned by
 * the caller's arena; no individual free is needed.
 */
typedef struct {
    LatVal *vals;  /**< Array of lattice values, one per tracked operand id. */
    int     size;  /**< Number of entries (equals VarMap::nextId at init time). */
} ConstMap;

/**
 * @brief Initialise a ConstMap with @p size entries, all set to UNKNOWN.
 *
 * Allocates @c vals from @p arena (lifetime is managed by the caller).
 *
 * @param m     ConstMap to initialise.
 * @param size  Number of entries (should equal VarMap::nextId).
 * @param arena Arena from which @c vals is allocated.
 */
void const_map_init(ConstMap *m, int size, Arena *arena);

/**
 * @brief Copy all entries from @p src into @p dst (both must have the same size).
 *
 * @param dst  Destination ConstMap (overwritten).
 * @param src  Source ConstMap (unchanged).
 */
void const_map_copy(ConstMap *dst, const ConstMap *src);

/**
 * @brief Return non-zero if @p a and @p b have identical lattice values
 *        for every tracked variable.
 *
 * Used by the fixed-point loop to detect convergence.
 *
 * @param a  First ConstMap.
 * @param b  Second ConstMap.
 * @return   1 if all entries are equal, 0 otherwise.
 */
int const_map_equal(const ConstMap *a, const ConstMap *b);

/**
 * @brief Apply the meet operation entry-wise: dest[i] = meet(dest[i], src[i]).
 *
 * Used at CFG join points to merge the Out sets of all predecessors into
 * the In set of the current block.
 *
 * @param dest  ConstMap updated in place (left operand of meet for each entry).
 * @param src   ConstMap providing the right operand (unchanged).
 */
void const_map_meet(ConstMap *dest, const ConstMap *src);

/**
 * @brief Return the lattice value associated with operand @p op.
 *
 * Resolves @p op through @p vm to obtain its variable id, then returns
 * @c m->vals[id].  Returns CONFLICT for operands with no valid id (constants,
 * labels, functions) — the caller should use lat_get_value_from_operand() for those instead.
 *
 * @param m   ConstMap to query.
 * @param op  Operand to look up.
 * @param vm  VarMap providing the operand→id mapping.
 * @return    The current LatVal for @p op, or CONFLICT if not found.
 */
LatVal const_map_get(const ConstMap *m, Operand op, VarMap *vm);

/**
 * @brief Like lat_get_value_from_operand(), but takes an already-resolved
 *        VarMap id instead of doing the hash lookup itself.
 *
 * For OPND_VAR/OPND_TEMP the caller must have obtained @p cachedId from
 * the operand's CURRENT kind (e.g. via VarMap's per-instruction id cache,
 * guarded by ir_operand_is_storage on the live operand) — this function
 * does not re-derive it. Literal operands ignore @p cachedId entirely.
 *
 * @param map        Current ConstMap.
 * @param op         Operand to lift (its kind decides the strategy).
 * @param cachedId   Precomputed id for @p op when it's VAR/TEMP; ignored otherwise.
 * @return           Lattice value for @p op.
 */
LatVal lat_get_value_by_id(const ConstMap *map, Operand op, int cachedId);

/**
 * @brief Like const_map_try_fold(), but takes an already-resolved VarMap id.
 *
 * @param op        Operand to attempt folding.
 * @param m         ConstMap providing the current lattice state.
 * @param cachedId  Precomputed id for @p op when it's VAR/TEMP; ignored otherwise.
 * @return          A constant operand if folding succeeded, @p op otherwise.
 */
Operand const_map_try_fold_by_id(Operand op, const ConstMap *m, int cachedId);

/**
 * @brief Constant-fold a unary operation (NEG or NOT) on a LatVal.
 *
 * Returns CONFLICT if @p v is not CONST.  For NOT applied to a float,
 * the result is an integer (0 or 1), following C's semantics for logical not.
 *
 * @param op  Unary IR opcode (IR_NEG or IR_NOT).
 * @param v   Lattice value of the single source operand.
 * @return    A CONST LatVal with the folded result, or CONFLICT on failure.
 */
LatVal lat_fold_unary(IROp op, LatVal v);

#endif /* CONSTMAP_H */
