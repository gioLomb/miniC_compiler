#include "bucket.h"
#include "arena.h"
#include <string.h>

/* Module-level arena */
static Arena *sArena = NULL;


Buckets buckets_create(int nextVreg, int nBuckets) {
    // Destroy any leftover arena from a previous (incorrectly unpaired) call.
    if (sArena) {
        arena_destroy(sArena);
        sArena = NULL;
    }
    sArena = arena_create(0);

    Buckets b;
    b.nBuckets     = nBuckets;
    b.nonempty = 0;

    int nodesNum = (nextVreg > 0) ? nextVreg : 1;   // guard against zero-size allocation

    b.head = arena_alloc(sArena, (size_t)nBuckets * sizeof(int));
    memset(b.head, -1, (size_t)nBuckets * sizeof(int));   // -1 = empty sentinel for each bucket

    b.next     = arena_alloc(sArena, (size_t)nodesNum * sizeof(int));
    b.prev     = arena_alloc(sArena, (size_t)nodesNum * sizeof(int));
    b.inBucket = arena_alloc(sArena, (size_t)nodesNum * sizeof(int));
    memset(b.inBucket, 0, (size_t)nodesNum * sizeof(int));

    return b;
}

void buckets_free() {
    //(void)b;   // arrays are owned by sArena; the parameter exists for API symmetry
    if (sArena) {
        arena_destroy(sArena);
        sArena = NULL;
    }
}


void bucket_insert(Buckets *b, int node, int degree) {
    // Prepend node to the doubly-linked list of bucket degree.
    int h = b->head[degree];
    b->prev[node] = -1;
    b->next[node] = h;
    if (h >= 0) b->prev[h] = node; // update old head's back-pointer
    b->head[degree] = node;
    b->inBucket[node] = 1;
    b->nonempty |= (1u << degree); // mark bucket degree as non-empty
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
    // non-empty bucket without scanning all nBucketsentries.
    *outDegree = __builtin_ctz(b->nonempty);
    return b->head[*outDegree];
}
