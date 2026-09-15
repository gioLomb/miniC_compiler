

#ifndef BUCKET_H
#define BUCKET_H


/**
 * @file bucket.h
 * @brief Degree-indexed bucket list for O(1) node extraction in graph coloring.
 *
 * Implements the classic Briggs/Chaitin work-list structure that keeps
 * nodes ordered by current degree so that the Simplify phase of register
 * allocation can pick a low-degree node in constant time.
 */

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
    int      nBuckets;         /**< Number of buckets (= PHYS_ALLOCATABLE).               */
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
 * @param nBuckets       Number of buckets (should equal PHYS_ALLOCATABLE).
 * @return         Initialised Buckets value; all buckets are empty.
 */
Buckets buckets_create(int nextVreg, int nBuckets);

/**
 * @brief Destroy the Buckets structure and release the internal arena.
 */
void buckets_free();

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
