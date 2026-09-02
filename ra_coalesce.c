#include <stdlib.h>
#include <stdint.h>
#include "ra_coalesce.h"

/* =========================================================================
 * Triangular-matrix edge query — mirrors interference.c
 * ========================================================================= */

/**
 * Map the unordered pair (i, j) to its flat lower-triangular index.
 * Canonical form enforced: larger id becomes the row (i > j after swap).
 */
static inline long tri_idx(int i, int j) {
    if (i < j) { 
        int t = i; 
        i = j; 
        j = t; 
    }
    // Standard lower-triangular formula: row i starts at i*(i-1)/2
    return (long)i * (i - 1) / 2 + j;
}

/** 
 * Return non-zero if an interference edge exists between nodes @p i and @p j. 
 */
static inline int edge_exists(const IGraph *g, int i, int j) {
    // Reject degenerate or self-pairs before touching the bit matrix
    if (i < 0 || j < 0 || i == j) return 0;
    
    long idx = tri_idx(i, j);
    // Word index (idx >> 6) selects the uint64_t, (idx & 63) the bit within it
    return (int)((g->matrix[idx >> 6] >> (idx & 63)) & 1ULL);
}


/**
 * Resolves a Machine Operand to its internal register node ID.
 * Returns -1 if the operand is not a valid register representation.
 */
static inline int resolve_operand_id(const MachOperand *op, int next_vreg) {
    if (op->kind == MO_VREG) {
        return op->vregId;
    }
    if (op->kind == MO_PHYS) {
        return next_vreg + op->physReg;
    }
    return -1;
}

/**
 * Validates whether a pair (u, v) is eligible for register coalescing.
 * Applies domain rules and interference checks.
 * (Refactoring: Replace Nested Conditional with Guard Clauses)
 */
static inline int is_valid_coalesce_candidate(const IGraph *g, int u, int v, int max_node_id, int next_vreg) {
    // Both operands must resolve to valid node ids
    if (u < 0 || v < 0) return 0;

    // Guard against out-of-range ids (beyond allocatable limits)
    if (u >= max_node_id || v >= max_node_id) return 0;

    // Phys <-> phys MOVs are never touched by the register allocator
    if (u >= next_vreg && v >= next_vreg) return 0;

    // Only record pairs that do NOT already interfere
    if (edge_exists(g, u, v)) return 0;

    return 1;
}


/** Append the pair (u, v) to @p pl, doubling capacity when needed. */
static void pl_push(PartnerList *pl, int u, int v) {
    // Start at 16 to amortise early reallocations on small functions
    if (pl->count == pl->cap) {
        pl->cap   = pl->cap ? pl->cap * 2 : 16;
        pl->pairs = realloc(pl->pairs, (size_t)pl->cap * sizeof(PartnerPair));
    }
    pl->pairs[pl->count++] = (PartnerPair){ u, v };
}


PartnerList ra_collect_partners(const MachFunction *f, const IGraph *g, int nextVreg) {
    PartnerList pl = { NULL, 0, 0 };
    
    // Refactoring: Extract Variable & Slide Statements outside hot loop
    const int max_node_id = nextVreg + PHYS_ALLOCATABLE;

    for (int i = 0; i < f->count; i++) {
        const MachInstr *in = &f->instrs[i];
        if (in->op != MACH_MOV) continue;

        // Refactoring: Extract Function calls replacing duplicated if-else trees
        int u = resolve_operand_id(&in->dst, nextVreg);
        int v = resolve_operand_id(&in->src1, nextVreg);

        // Refactoring: Replace Nested Conditional with Guard Clauses
        if (is_valid_coalesce_candidate(g, u, v, max_node_id, nextVreg)) {
            pl_push(&pl, u, v);
        }
    }

    return pl;
}

void partnerlist_free(PartnerList *pl) {
    if (!pl) return;
    
    free(pl->pairs);
    // Reset all fields so a double-free attempt produces a clean no-op
    pl->pairs = NULL;
    pl->count = 0;
    pl->cap   = 0;
}