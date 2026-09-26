#ifndef DYNAMIC_ARRAY_H
#define DYNAMIC_ARRAY_H

/**
 * @file dynamic_array.h
 * @brief Generic resizable integer array (IntVector).
 *
 * Simple growable array of ints. Backing storage is either heap (malloc)
 * or an Arena: arena-backed vectors never free individual buffers — the
 * arena reclaims them in bulk. Used as a lightweight container for
 * instruction lists, successor sets, work-lists and interference-graph
 * adjacency lists.
 */

typedef struct Arena Arena;

/** Initial capacity on the first push (avoids realloc on every early push). */
#define INT_VECTOR_INITIAL_CAP 16

/**
 * @brief Resizable array of @c int values.
 *
 * Invariant: @c len ≤ @c cap.  When @c cap == 0, @c data is NULL.
 * Never access @c data[i] for @c i ≥ @c len.
 * If @c arena is non-NULL, growth allocates from that arena and
 * @c int_vector_free is a no-op on the backing buffer.
 */
typedef struct {
    int *data;      /**< Backing array; NULL when cap == 0. */
    int  len;       /**< Number of elements currently stored. */
    int  cap;       /**< Allocated capacity (number of slots in @c data). */
    Arena *arena;   /**< Non-NULL → bump-allocate growth; never free data. */
} IntVector;

/**
 * @brief Initialise an IntVector, optionally pre-reserving heap capacity.
 *
 * @param v         IntVector to initialise (must not be NULL).
 * @param capacity  Initial capacity to reserve. Pass 0 for lazy allocation.
 */
void int_vector_init(IntVector *v, int capacity);

/**
 * @brief Initialise an IntVector that grows from @p arena.
 *
 * Old buffers are abandoned in the arena on growth (same amortised waste
 * as other arena-backed growable arrays in this compiler).
 */
void int_vector_init_arena(IntVector *v, int capacity, Arena *arena);

/**
 * @brief Append @p value to the end of @p v, growing the backing array if needed.
 *
 * Uses a doubling strategy.  Calls @c abort() on allocation failure.
 */
void int_vector_push(IntVector *v, int value);

/**
 * @brief Free the backing array (heap vectors only) and reset fields.
 *
 * Arena-backed vectors only zero the header — the arena owns the bytes.
 */
void int_vector_free(IntVector *v);

/**
 * @brief Reset the element count to zero without freeing the backing array.
 */
void int_vector_clear(IntVector *v);

#endif /* DYNAMIC_ARRAY_H */
