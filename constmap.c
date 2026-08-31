/**
 * @file constmap.c
 * @brief Constant-propagation lattice and ConstMap implementation.
 *
 * See constmap.h for the module overview, lattice definition, and full API
 * documentation.  This file contains only implementation details.
 *
 * Representation notes
 * --------------------
 * LatVal is a small POD struct (12 bytes on typical 32-bit-aligned targets)
 * stored by value throughout.  Returning and passing LatVal by value avoids
 * heap allocation and pointer indirection in the hot dataflow loop in cp.c.
 *
 * ConstMap is a flat array of LatVal allocated entirely from the caller's
 * Arena.  Indexed access is O(1) and the array is contiguous in memory,
 * which benefits the entry-wise meet (const_map_meet) and equality check
 * (const_map_equal) that iterate over all entries in the fixed-point loop.
 *
 * Division-by-zero policy
 * -----------------------
 * Both fold_binary_int and fold_binary_float return 0 (fold not performed) when
 * the divisor is zero.  This defers the division to runtime, where it will
 * either raise a signal or produce implementation-defined behaviour — the
 * same outcome the unoptimised code would produce.  Folding a division by
 * zero to an arbitrary constant would silently change program semantics.
 */

#include "constmap.h"
#include "arena.h"
#include <string.h>

/* =========================================================================
 * Lattice constructors
 * ========================================================================= */

LatVal lat_unknown(void) {
    LatVal v = {0};
    v.state = LAT_UNKNOWN;
    return v;
}

LatVal lat_set_const_int(int ival) {
    LatVal v = {0};
    v.state      = LAT_CONST;
    v.isFloat    = 0;
    v.val.ival   = ival;
    return v;
}

LatVal lat_set_const_float(float fval) {
    LatVal v = {0};
    v.state      = LAT_CONST;
    v.isFloat    = 1;
    v.val.fval   = fval;
    return v;
}

LatVal lat_conflict(void) {
    LatVal v = {0};
    v.state = LAT_CONFLICT;
    return v;
}

/* =========================================================================
 * Lattice predicates
 * ========================================================================= */

int lat_is_const(LatVal v)    { return v.state == LAT_CONST;    }
int lat_is_unknown(LatVal v)  { return v.state == LAT_UNKNOWN;  }
int lat_is_conflict(LatVal v) { return v.state == LAT_CONFLICT; }

/* =========================================================================
 * Lattice meet (⊓)
 * =========================================================================
 * The meet of two lattice elements represents the information that is
 * guaranteed to hold on ALL paths reaching a join point.  The ordering
 * is UNKNOWN (most information: "not yet seen") > CONST > CONFLICT (least
 * information: "could be anything").  UNKNOWN is the identity because
 * meeting with "no information" leaves the other operand unchanged.
 * CONFLICT is absorbing because once a variable's value is uncertain it
 * can never become certain again within a single dataflow pass.
 * ========================================================================= */

/**
 * @brief Compute the meet (⊓) of two lattice elements.
 *
 * Implementation follows the lattice ordering directly: UNKNOWN is the
 * identity, CONFLICT is absorbing.  Two CONST elements with the same type
 * and value meet to themselves; any mismatch (different numeric values or
 * different types, e.g. int vs float) meets to CONFLICT.
 */
LatVal lat_meet(LatVal a, LatVal b) {
    // UNKNOWN is the identity element: UNKNOWN ⊓ x = x
    if (a.state == LAT_UNKNOWN) return b;
    if (b.state == LAT_UNKNOWN) return a;

    // CONFLICT is absorbing: CONFLICT ⊓ x = CONFLICT
    if (a.state == LAT_CONFLICT || b.state == LAT_CONFLICT) return lat_conflict();

    // Both are CONST: they must agree on type and value to remain CONST
    if (a.isFloat != b.isFloat) return lat_conflict(); // int ⊓ float → ambiguous type
    if (a.isFloat) {
        if (a.val.fval == b.val.fval) return a;
    } else {
        if (a.val.ival == b.val.ival) return a;
    }
    return lat_conflict(); // same type but different values → not a single constant
}

/**
 * @brief Return non-zero if @p a and @p b represent the same lattice element.
 *
 * Equality is used by the fixed-point loop to detect convergence: the loop
 * terminates when no Out set changes between two successive iterations.
 */
int lat_equal(LatVal a, LatVal b) {
    if (a.state != b.state) return 0;
    if (a.state != LAT_CONST) return 1; // both UNKNOWN or both CONFLICT: always equal
    if (a.isFloat != b.isFloat) return 0;
    return a.isFloat ? (a.val.fval == b.val.fval) : (a.val.ival == b.val.ival);
}

/* =========================================================================
 * ConstMap operations
 * ========================================================================= */

/**
 * @brief Initialise a ConstMap, setting all entries to UNKNOWN.
 *
 * Allocating from the caller's arena avoids per-map malloc/free pairs and
 * lets the caller reclaim all CP-related memory in a single arena_destroy().
 */
void const_map_init(ConstMap *m, int size, Arena *arena) {
    m->size = size;
    m->vals = arena_alloc(arena, (size_t)size * sizeof(LatVal));
    // initialise every entry to UNKNOWN: no definitions seen yet on any path
    for (int i = 0; i < size; i++) m->vals[i] = lat_unknown();
}

void const_map_copy(ConstMap *dst, const ConstMap *src) {
    memcpy(dst->vals, src->vals, (size_t)src->size * sizeof(LatVal));
}

/**
 * @brief Return non-zero if every entry in @p a equals the corresponding
 *        entry in @p b.
 *
 * Short-circuits on the first mismatch for efficiency in the common case
 * where the fixed-point has not yet been reached and many entries differ.
 */
int const_map_equal(const ConstMap *a, const ConstMap *b) {
    for (int i = 0; i < a->size; i++)
        if (!lat_equal(a->vals[i], b->vals[i])) return 0;
    return 1;
}

/**
 * @brief Apply the meet operation entry-wise to merge predecessor Out sets.
 *
 * Called once per predecessor at each join point: the first predecessor
 * copies its Out into In[b] (const_map_copy), and each subsequent one applies
 * this function so that In[b] accumulates the greatest lower bound.
 */
void const_map_meet(ConstMap *dest, const ConstMap *src) {
    for (int i = 0; i < dest->size; i++)
        dest->vals[i] = lat_meet(dest->vals[i], src->vals[i]);
}

/**
 * @brief Return the lattice value for operand @p op from map @p m.
 *
 * Resolves @p op to an id via @p vm.  An id outside [0, m->size) indicates
 * an operand that was not tracked (e.g. added after the VarMap was built);
 * returning CONFLICT is the safe conservative choice.
 */
LatVal const_map_get(const ConstMap *m, Operand op, VarMap *vm) {
    int id = varmap_operand_id(vm, op);
    if (id < 0 || id >= m->size) return lat_conflict(); // id out of range → conservative
    return m->vals[id];
}

/**
 * @brief Substitute @p op with its constant value if one is known.
 *
 * Only variables and temporaries are candidates for substitution; constant
 * operands and non-storage kinds are returned unchanged.  When folding
 * succeeds, the returned operand carries the value inline (OPND_CONST_INT or
 * OPND_CONST_FLOAT), so the instruction selector never needs to load it from
 * a variable slot.
 */
Operand const_map_try_fold(Operand op, const ConstMap *m, VarMap *vm) {
    if (op.kind != OPND_VAR && op.kind != OPND_TEMP) return op; // not a storage location
    LatVal lv = const_map_get(m, op, vm);
    if (lv.state != LAT_CONST) return op; // UNKNOWN or CONFLICT: cannot substitute
    if (lv.isFloat) {
        Operand o;
        o.kind           = OPND_CONST_FLOAT;
        o.data.floatVal  = lv.val.fval;
        return o;
    } else {
        Operand o;
        o.kind         = OPND_CONST_INT;
        o.data.intVal  = lv.val.ival;
        return o;
    }
}

/* =========================================================================
 * Transfer function helpers
 * =========================================================================
 * These are called by cp.c's transferInstr() to evaluate the constant value
 * an instruction would produce given fully constant inputs.  They are also
 * used by the rewriting phase to fold operations whose operands have been
 * substituted with constants by const_map_try_fold().
 * ========================================================================= */

/**
 * @brief Lift an Operand to a LatVal, consulting @p map for variables.
 *
 * Literal operands (OPND_CONST_INT, OPND_CONST_FLOAT) are always CONST
 * regardless of the map; their values are available inline in the operand.
 * Variables and temporaries are looked up in @p map via @p vm.
 * All other kinds (label, function name, none) return CONFLICT because they
 * cannot carry a numeric constant that can be propagated.
 */
LatVal lat_get_value_from_operand(const ConstMap *map, Operand op, VarMap *vm) {
    switch (op.kind) {
    case OPND_CONST_INT:   return lat_set_const_int(op.data.intVal);     // inline integer constant
    case OPND_CONST_FLOAT: return lat_set_const_float(op.data.floatVal); // inline float constant
    case OPND_VAR:
    case OPND_TEMP:        return const_map_get(map, op, vm);         // look up in propagation state
    default:               return lat_conflict();                     // label, func, none: not a value
    }
}





/**
 * @brief Constant-fold a unary operation (NEG or NOT) on a lattice value.
 *
 * Returns CONFLICT immediately if @p v is not CONST — there is nothing to
 * fold when the operand's value is unknown or ambiguous.
 * IR_NOT applied to a float follows C semantics: the result is an integer
 * (0 or 1), stored as LAT_CONST with isFloat == 0.
 */
LatVal fold_unary(IROp op, LatVal v) {
    if (v.state != LAT_CONST) return lat_conflict(); // can only fold a known constant
    if (!v.isFloat) {
        int i = v.val.ival;
        if (op == IR_NEG) return lat_set_const_int(-i);
        if (op == IR_NOT) return lat_set_const_int(!i);  // logical not: 0→1, non-zero→0
    } else {
        float f = v.val.fval;
        if (op == IR_NEG) return lat_set_const_float(-f);
        // IR_NOT on float: result is int (C semantics: !0.0f == 1, !non-zero == 0)
        if (op == IR_NOT) return lat_set_const_int(f == 0.0f ? 1 : 0);
    }
    return lat_conflict(); // opcode not handled (should not occur in well-formed IR)
}
