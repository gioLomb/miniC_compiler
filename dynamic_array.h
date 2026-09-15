

#ifndef DYNAMIC_ARRAY_H
#define DYNAMIC_ARRAY_H


/**
 * @file dynamic_array.h
 * @brief Generic resizable integer array (IntVector).
 *
 * Simple growable array of ints allocated from an Arena. Used as a
 * lightweight container for instruction lists, successor sets, work-lists
 * and any other ordered collection of integer identifiers.
 */

/** Initial capacity on the first push (avoids realloc on every early push). */
#define INT_VECTOR_INITIAL_CAP 16

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
 * @brief Initialise an IntVector, optionally pre-reserving capacity.
 *
 * @param v         IntVector to initialise (must not be NULL).
 * @param capacity  Initial capacity to reserve. Pass 0 for the original
 *                  lazy behaviour (no allocation until the first push,
 *                  which then grows by doubling from INT_VECTOR_INITIAL_CAP).
 *                  Pass a positive value to allocate it upfront in one shot,
 *                  skipping the early doubling steps int_vector_push() would
 *                  otherwise trigger — useful when a reasonable upper bound
 *                  on the final size is known ahead of time (e.g. graph
 *                  degree bounded by node count). Growth beyond @p capacity
 *                  still falls back to normal doubling; correctness is
 *                  unaffected either way.
 */
void int_vector_init(IntVector *v, int capacity);

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