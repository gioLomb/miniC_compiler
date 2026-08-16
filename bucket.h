/**
 * @file bucket.h
 * @brief Degree-indexed bucket list for O(1) node extraction in graph coloring.
 *
 * Provides a fixed-width array of doubly-linked lists (one per degree 0..k-1)
 * used by the Briggs-optimistic Simplify phase of the Chaitin-Briggs register
 * allocator (ra_color.c).  The key invariant is that every active virtual
 * register sits in the bucket whose index equals its current interference-graph
 * degree, capped at k-1 (= PHYS_ALLOCATABLE - 1).
 *
 * Design
 * ------
 * Each bucket is a doubly-linked list threaded through the next/prev arrays,
 * allowing O(1) insert and remove without touching a heap allocator.  A 32-bit
 * bitmask (nonempty) records which buckets contain at least one node; the
 * lowest set bit is found in one instruction via __builtin_ctz, making
 * bucket_pop_any_low() O(1) as well.
 *
 * Ownership and lifetime
 * ----------------------
 * All storage (head, next, prev, inBucket) is allocated from a private
 * module-level arena created by buckets_create() and destroyed by
 * buckets_free().  Only one Buckets instance may exist at a time per process
 * (the arena is a module-level singleton).  This matches the single-threaded,
 * single-allocation-at-a-time usage pattern of the register allocator.
 */

#ifndef BUCKET_H
#define BUCKET_H

#include <stdint.h>

/**
 * @brief Degree-indexed bucket list for register-allocator Simplify.
 *
 * All pointer-like arrays are indexed by virtual register id (0..nextVreg-1).
 * The head[] array is indexed by degree (0..k-1).
 */
typedef struct {
    int     *head;      /**< head[degree]: first node in bucket degree, -1 if empty. */
    int     *next;      /**< next[node]: next node in the same bucket as node.        */
    int     *prev;      /**< prev[node]: previous node in the same bucket as node.    */
    int     *inBucket;  /**< inBucket[node]: 1 if node is currently in any bucket.    */
    int      k;         /**< Number of buckets (= PHYS_ALLOCATABLE).               */
    uint32_t nonempty;  /**< Bitmask: bit degree is set iff head[degree] != -1.    */
} Buckets;

/**
 * @brief Allocate and initialise a Buckets structure.
 *
 * Creates an internal arena, allocates head[], next[], prev[], and
 * inBucket[] from it, and initialises all entries.  Only one instance may
 * be live at a time (the arena is a module-level singleton).
 *
 * @param nextVreg Number of virtual registers (maximum node id + 1).
 * @param k        Number of buckets (should equal PHYS_ALLOCATABLE).
 * @return         Initialised Buckets value; all buckets are empty.
 */
Buckets buckets_create(int nextVreg, int k);

/**
 * @brief Destroy the Buckets structure and release the internal arena.
 *
 * The @p b parameter is accepted for API symmetry but the arrays it points
 * to are owned by the module-level arena, which is freed here.  @p b itself
 * must not be used after this call.
 *
 * @param b Pointer to the Buckets to destroy.
 */
void buckets_free(Buckets *b);

/**
 * @brief Insert node @p node into bucket @p degree.
 *
 * Prepends @p node to the doubly-linked list at head[degree] and sets bit degree in
 * nonempty.  Behaviour is undefined if @p node is already in a bucket.
 *
 * @param b      Buckets structure to update.
 * @param node   Node (virtual register id) to insert.
 * @param degree Target bucket index (0 <= degree < b->k).
 */
void bucket_insert(Buckets *b, int node, int degree);

/**
 * @brief Remove node @p node from bucket @p degree.
 *
 * Unlinks @p node from the doubly-linked list at head[degree] and clears bit degree in
 * nonempty if the bucket becomes empty.  Behaviour is undefined if @p node is
 * not currently in bucket @p degree.
 *
 * @param b      Buckets structure to update.
 * @param node   Node to remove.
 * @param degree Bucket index that currently contains @p node.
 */
void bucket_remove(Buckets *b, int node, int degree);

/**
 * @brief Extract the head node from the lowest non-empty bucket.
 *
 * Uses __builtin_ctz on the nonempty bitmask for O(1) lookup of the
 * minimum-degree bucket.  The node is removed from its bucket before
 * being returned.
 *
 * @param b        Buckets structure.
 * @param outDegree Set to the index of the bucket from which the node was taken.
 * @return         The extracted node id, or -1 if all buckets are empty.
 */
int bucket_pop_any_low(Buckets *b, int *outDegree);

#endif /* BUCKET_H */
