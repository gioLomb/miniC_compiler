/**
 * @file dynamic_array.h
 * @brief Generic resizable integer array (IntVector).
 *
 * Provides a minimal growable array of @c int values used across several
 * modules that need variable-length lists whose size is not known at
 * allocation time:
 *
 *   - interference.c  — adjacency lists of the interference graph
 *                       (typedef'd as AdjList)
 *   - licm.c          — per-variable use lists during invariant detection
 *                       (typedef'd as UsedByList)
 *
 * The implementation uses a standard doubling strategy: the backing array
 * starts empty and is reallocated to twice its capacity whenever a push
 * would exceed it.  All memory is heap-allocated (malloc/realloc/free);
 * IntVector does not interact with any Arena.
 */

#ifndef DYNAMIC_ARRAY_H
#define DYNAMIC_ARRAY_H

/**
 * @brief Resizable array of @c int values.
 *
 * Invariant: @c len ≤ @c cap.  When @c cap == 0, @c data is NULL.
 * Never access @c data[i] for @c i ≥ @c len.
 */
typedef struct {
    int *data;  /**< Heap-allocated backing array; NULL when cap == 0. */
    int  len;   /**< Number of elements currently stored.              */
    int  cap;   /**< Allocated capacity (number of slots in @c data).  */
} IntVector;

/**
 * @brief Initialise an empty IntVector.
 *
 * Sets all fields to zero/NULL.  Must be called before any other
 * IntVector operation.  Pairs with @c int_vector_free().
 *
 * @param v  IntVector to initialise (must not be NULL).
 */
void int_vector_init(IntVector *v);

/**
 * @brief Append @p value to the end of @p v, growing the backing array if needed.
 *
 * Uses a doubling strategy: when @c len == @c cap, capacity is doubled
 * (or set to an initial minimum if currently zero).  Calls @c abort() on
 * allocation failure — consistent with the rest of the compiler's OOM policy.
 *
 * @param v      IntVector to append to (must not be NULL).
 * @param value  Integer value to append.
 */
void int_vector_push(IntVector *v, int value);

/**
 * @brief Free the backing array and reset all fields to zero/NULL.
 *
 * After this call @p v is in the same state as after @c int_vector_init().
 * Calling @c int_vector_free() on an already-empty vector is safe (no-op).
 *
 * @param v  IntVector to free (must not be NULL).
 */
void int_vector_free(IntVector *v);

/**
 * @brief Reset the element count to zero without freeing the backing array.
 *
 * Retains the allocated capacity for reuse, avoiding a reallocation on the
 * next sequence of pushes.  Equivalent to @c v->len = 0 but preferred for
 * clarity at call sites.
 *
 * @param v  IntVector to clear (must not be NULL).
 */
void int_vector_clear(IntVector *v);

#endif /* DYNAMIC_ARRAY_H */