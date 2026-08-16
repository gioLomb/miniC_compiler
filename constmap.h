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
 *   lat_getValueFromOperand — lift an Operand to a LatVal (inline constants or map lookup)
 *   isBinaryOp      — predicate: opcode takes two operands and produces a value
 *   isComparisonOp  — predicate: opcode is a relational comparison
 *   foldBinaryInt   — constant-fold a binary integer operation
 *   foldBinaryFloat — constant-fold a binary float operation
 *   foldUnary       — constant-fold NEG or NOT
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

/** @brief Return the UNKNOWN (⊤) lattice element. */
LatVal lat_unknown(void);

/** @brief Return a CONST lattice element wrapping the integer @p ival. */
LatVal lat_setConstInt(int ival);

/** @brief Return a CONST lattice element wrapping the float @p fval. */
LatVal lat_setConstFloat(float fval);

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
int lat_isConst(LatVal v);

/** @brief Return non-zero if @p v is the UNKNOWN lattice element. */
int lat_isUnknown(LatVal v);

/** @brief Return non-zero if @p v is the CONFLICT lattice element. */
int lat_isConflict(LatVal v);

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
void constMap_init(ConstMap *m, int size, Arena *arena);

/**
 * @brief Copy all entries from @p src into @p dst (both must have the same size).
 *
 * @param dst  Destination ConstMap (overwritten).
 * @param src  Source ConstMap (unchanged).
 */
void constMap_copy(ConstMap *dst, const ConstMap *src);

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
int constMap_equal(const ConstMap *a, const ConstMap *b);

/**
 * @brief Apply the meet operation entry-wise: dest[i] = meet(dest[i], src[i]).
 *
 * Used at CFG join points to merge the Out sets of all predecessors into
 * the In set of the current block.
 *
 * @param dest  ConstMap updated in place (left operand of meet for each entry).
 * @param src   ConstMap providing the right operand (unchanged).
 */
void constMap_meet(ConstMap *dest, const ConstMap *src);

/**
 * @brief Return the lattice value associated with operand @p op.
 *
 * Resolves @p op through @p vm to obtain its variable id, then returns
 * @c m->vals[id].  Returns CONFLICT for operands with no valid id (constants,
 * labels, functions) — the caller should use lat_getValueFromOperand() for those instead.
 *
 * @param m   ConstMap to query.
 * @param op  Operand to look up.
 * @param vm  VarMap providing the operand→id mapping.
 * @return    The current LatVal for @p op, or CONFLICT if not found.
 */
LatVal constMap_get(const ConstMap *m, Operand op, VarMap *vm);

/**
 * @brief If @p op has a known constant value in @p m, return a folded operand.
 *
 * When @p op is a variable or temporary whose lattice value is CONST, returns
 * a new OPND_CONST_INT or OPND_CONST_FLOAT operand carrying the constant
 * inline.  Otherwise returns @p op unchanged.  Used by the CP rewriting phase
 * to substitute variables with their constant values in place.
 *
 * @param op  Operand to attempt folding.
 * @param m   ConstMap providing the current lattice state.
 * @param vm  VarMap for id resolution.
 * @return    A constant operand if folding succeeded, @p op otherwise.
 */
Operand constMap_tryFold(Operand op, const ConstMap *m, VarMap *vm);

/* =========================================================================
 * Transfer function helpers
 * =========================================================================
 * These functions are called by cp.c's transferInstr() to evaluate whether
 * an IR instruction produces a constant result given the current ConstMap.
 * They are declared here (rather than being static in cp.c) so that dce.c
 * can reuse the folding kernels without duplicating the arithmetic logic.
 * ========================================================================= */

/**
 * @brief Lift an Operand to a LatVal, consulting @p map for variables.
 *
 * Dispatch rules:
 *   - OPND_CONST_INT   → CONST wrapping the inline integer value
 *   - OPND_CONST_FLOAT → CONST wrapping the inline float value
 *   - OPND_VAR / OPND_TEMP → constMap_get() lookup
 *   - anything else (label, func, none) → CONFLICT (not a constant source)
 *
 * @param map  Current ConstMap.
 * @param op   Operand to lift.
 * @param vm   VarMap for variable id resolution.
 * @return     The LatVal representing @p op's current compile-time value.
 */
LatVal lat_getValueFromOperand(const ConstMap *map, Operand op, VarMap *vm);

/**
 * @brief Return non-zero if @p op is a binary arithmetic or relational opcode.
 *
 * @param op  IR opcode to test.
 * @return    1 if @p op takes two source operands and produces a value.
 */
int isBinaryOp(IROp op);

/**
 * @brief Return non-zero if @p op is a relational comparison opcode.
 *
 * Used to decide whether a folded float result should be stored as int (0/1).
 *
 * @param op  IR opcode to test.
 * @return    1 if @p op is one of LT, LE, GT, GE, EQ, NE.
 */
int isComparisonOp(IROp op);

/**
 * @brief Constant-fold a binary operation on two integer values.
 *
 * Evaluates @c a op b at compile time and writes the result into @p res.
 * Division and modulo by zero are handled safely by returning 0 (not folded)
 * rather than invoking undefined behaviour; the fold is deferred to runtime.
 *
 * @param op   Binary IR opcode (ADD, SUB, MUL, DIV, MOD, or a comparison).
 * @param a    Left operand integer value.
 * @param b    Right operand integer value.
 * @param res  Receives the folded result when the function returns 1.
 * @return     1 if folding succeeded, 0 if the operation cannot be folded
 *             (unsupported opcode or division/modulo by zero).
 */
int foldBinaryInt(IROp op, int a, int b, int *res);

/**
 * @brief Constant-fold a binary operation on two float values.
 *
 * Analogous to foldBinaryInt() but for floating-point operands.
 * Float division by zero is guarded the same way as integer division.
 * Comparison opcodes produce a float result of 0.0 or 1.0; the caller is
 * responsible for converting to int when storing a comparison result.
 *
 * @param op   Binary IR opcode.
 * @param a    Left operand float value.
 * @param b    Right operand float value.
 * @param res  Receives the folded result when the function returns 1.
 * @return     1 if folding succeeded, 0 otherwise.
 */
int foldBinaryFloat(IROp op, float a, float b, float *res);

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
LatVal foldUnary(IROp op, LatVal v);

#endif /* CONSTMAP_H */