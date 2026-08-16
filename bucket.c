/**
 * @file bucket.c
 * @brief Degree-indexed bucket list — implementation.
 *
 * All storage is allocated from a module-level arena created by
 * buckets_create() and released by buckets_free().  The arena avoids
 * per-array malloc/free pairs and guarantees a single contiguous
 * allocation for the lifetime of one register-allocation round.
 */

#include "bucket.h"
#include "arena.h"
#include <string.h>

/* =========================================================================
 * Module-level arena
 * =========================================================================
 * Owns the memory for head[], next[], prev[], and inBucket[].
 * Only one Buckets instance may be live at a time; buckets_create()
 * enforces this by destroying any leftover arena before creating a new one.
 * ========================================================================= */
static Arena *s_arena = NULL;

/* =========================================================================
 * Public API
 * ========================================================================= */

Buckets buckets_create(int nextVreg, int k) {
    // Destroy any leftover arena from a previous (incorrectly unpaired) call.
    if (s_arena) {
        arena_destroy(s_arena);
        s_arena = NULL;
    }
    s_arena = arena_create(0);

    Buckets b;
    b.k        = k;
    b.nonempty = 0;

    int nodesNum = (nextVreg > 0) ? nextVreg : 1;   // guard against zero-size allocation

    b.head = arena_alloc(s_arena, (size_t)k * sizeof(int));
    memset(b.head, -1, (size_t)k * sizeof(int));   // -1 = empty sentinel for each bucket

    b.next     = arena_alloc(s_arena, (size_t)nodesNum * sizeof(int));
    b.prev     = arena_alloc(s_arena, (size_t)nodesNum * sizeof(int));
    b.inBucket = arena_alloc(s_arena, (size_t)nodesNum * sizeof(int));
    memset(b.inBucket, 0, (size_t)nodesNum * sizeof(int));

    return b;
}

void buckets_free(Buckets *b) {
    (void)b;   // arrays are owned by s_arena; the parameter exists for API symmetry
    if (s_arena) {
        arena_destroy(s_arena);
        s_arena = NULL;
    }
}

void bucket_insert(Buckets *b, int node, int degree) {
    // Prepend node to the doubly-linked list of bucket degree.
    b->prev[node] = -1;
    b->next[node] = b->head[degree];
    if (b->head[degree] >= 0)
        b->prev[b->head[degree]] = node;   // update old head's back-pointer
    b->head[degree]     = node;
    b->inBucket[node]   = 1;
    b->nonempty        |= (1u << degree);  // mark bucket degree as non-empty
}

void bucket_remove(Buckets *b, int node, int degree) {
    // Unlink node from the doubly-linked list of bucket degree.
    if (b->prev[node] >= 0)
        b->next[b->prev[node]] = b->next[node];
    else
        b->head[degree] = b->next[node];   // node was the head; promote its successor

    if (b->next[node] >= 0)
        b->prev[b->next[node]] = b->prev[node];

    b->inBucket[node] = 0;
    if (b->head[degree] < 0)
        b->nonempty &= ~(1u << degree);  // bucket degree is now empty; clear its bit
}

int bucket_pop_any_low(Buckets *b, int *outDegree) {
    if (!b->nonempty) return -1;

    // __builtin_ctz finds the lowest set bit in O(1), giving the minimum-degree
    // non-empty bucket without scanning all k entries.
    *outDegree = __builtin_ctz(b->nonempty);
    return b->head[*outDegree];
}