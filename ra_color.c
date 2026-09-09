#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <float.h>
#include "ra_color.h"
#include "ra_coalesce.h"
#include "instr_selector.h"   /* PHYS_ALLOCATABLE, PHYS_CALLER_SAVED_COUNT, PHYS_RBX..R15 */

/**
 * Scans active, non-bucketed nodes to find the best optimistic spill candidate.
 * Strictly prefers real IR variables over reload temporaries.
 */
static int ra_select_spill_candidate(const IGraph *g, int nextVreg, const Buckets *buckets) {
    double bestRealRatio = DBL_MAX, bestReloadRatio = DBL_MAX;
    int bestReal = -1, bestReload = -1;

    for (int v = 0; v < nextVreg; v++) {
        if (!g->active[v] || buckets->inBucket[v]) continue;

        double ratio = (g->degree[v] > 0)
                       ? (double)g->spillCost[v] / g->degree[v]
                       : 0.0;

        if (g->isReloadTemp[v]) {
            if (ratio < bestReloadRatio) { 
                bestReloadRatio = ratio; 
                bestReload = v; 
            }
        } else {
            if (ratio < bestRealRatio) { 
                bestRealRatio = ratio; 
                bestReal = v; 
            }
        }
    }

    // Prefer a real candidate; fall back to reload temp to guarantee progress
    return (bestReal >= 0) ? bestReal : bestReload;
}

/**
 * Decrements the degrees of active neighbors after removing a node,
 * adjusting their bucket status accordingly.
 */
static inline void ra_update_neighbor_degrees(IGraph *g, int chosen, int nextVreg, int k, Buckets *buckets) {
    for (int idx = 0; idx < g->adj[chosen].len; idx++) {
        int w = g->adj[chosen].data[idx];
        if (w >= nextVreg || !g->active[w]) continue;

        int oldDeg = g->degree[w];
        g->degree[w]--;

        if (oldDeg < k) {
            bucket_remove(buckets, w, oldDeg);
            bucket_insert(buckets, w, oldDeg - 1);
        } else if (oldDeg == k) {
            bucket_insert(buckets, w, k - 1);
        }
    }
}

/* =========================================================================
 * ra_simplify — Briggs-optimistic simplification.
 * ========================================================================= */
int ra_simplify(IGraph *g, int nextVreg, int **outStack)
{
    int stackCap = (nextVreg > 0) ? nextVreg : 1;
    *outStack = malloc((size_t)stackCap * sizeof(int));

    int stackLen   = 0;
    const int k    = PHYS_ALLOCATABLE;
    Buckets buckets = buckets_create(nextVreg, k);
    int remaining  = nextVreg;

    for (int v = 0; v < nextVreg; v++) {
        if (g->degree[v] < k) {
            bucket_insert(&buckets, v, g->degree[v]);
        }
    }

    while (remaining > 0) {
        int degree;
        int chosen = bucket_pop_any_low(&buckets, &degree);
        int fromBucket = (chosen >= 0);

        if (!fromBucket) {
            // No node has degree < k: select optimistic spill candidate
            chosen = ra_select_spill_candidate(g, nextVreg, &buckets);
            if (chosen < 0) break; // Impossible to proceed

            degree = (g->degree[chosen] < k) ? g->degree[chosen] : (k - 1);
        } else {
            bucket_remove(&buckets, chosen, degree);
        }

        g->active[chosen] = 0;
        remaining--;
        (*outStack)[stackLen++] = chosen;

        ra_update_neighbor_degrees(g, chosen, nextVreg, k, &buckets);
    }

    buckets_free();
    return stackLen;
}



/**
 * @brief CSR (compressed sparse row) index: node id -> list of partner node ids.
 *
 * Built once per ra_select_colors() call from the flat PartnerList, replacing
 * the O(pl->count) linear scan ra_hint_color() previously did for every single
 * node being colored (O(nodes * moves) worst case over the whole function).
 * Lookup for one node becomes O(local degree) instead of O(total pairs).
 */
typedef struct {
    int *start;  /**< start[v]: offset into data[] for node v; start[n] = total. */
    int *data;   /**< Flat array of partner node ids, one contiguous slice per node. */
} PartnerIndex;

/**
 * @brief Build the node -> partners CSR index from @p pl.
 *
 * Each PartnerPair(u,v) contributes one entry to u's list and one to v's
 * list (undirected: coloring either endpoint may want to check the other).
 * Entries within a node's slice preserve @p pl's original pair order, so
 * scan-order-dependent tie-breaking (first valid hint wins) is unaffected.
 *
 * @param pl  Partner list to index; NULL or empty yields an empty index.
 * @param n   Total node count (== IGraph.n), sizes the start[] array.
 * @return    Heap-allocated PartnerIndex; release with partner_index_free().
 */
static PartnerIndex ra_build_partner_index(const PartnerList *pl, int n) {
    PartnerIndex idx = { NULL, NULL };
    if (!pl || pl->count == 0 || n <= 0) return idx;

    // pass 1: count how many pairs touch each node
    int *count = calloc((size_t)n, sizeof(int));
    for (int i = 0; i < pl->count; i++) {
        int u = pl->pairs[i].u, v = pl->pairs[i].v;
        if (u >= 0 && u < n) count[u]++;
        if (v >= 0 && v < n) count[v]++;
    }

    // pass 2: prefix sum -> offsets
    idx.start = malloc((size_t)(n + 1) * sizeof(int));
    int total = 0;
    for (int i = 0; i < n; i++) { idx.start[i] = total; total += count[i]; }
    idx.start[n] = total;

    // pass 3: scatter, using a write cursor seeded from start[]
    idx.data = malloc((size_t)(total > 0 ? total : 1) * sizeof(int));
    int *cursor = malloc((size_t)n * sizeof(int));
    memcpy(cursor, idx.start, (size_t)n * sizeof(int));

    for (int i = 0; i < pl->count; i++) {
        int u = pl->pairs[i].u, v = pl->pairs[i].v;
        if (u >= 0 && u < n && v >= 0 && v < n) {
            idx.data[cursor[u]++] = v;
            idx.data[cursor[v]++] = u;
        }
    }

    free(cursor);
    free(count);
    return idx;
}

/** @brief Release a PartnerIndex built by ra_build_partner_index(). */
static void partner_index_free(PartnerIndex *idx) {
    free(idx->start);
    free(idx->data);
    idx->start = idx->data = NULL;
}

/**
 * Look up a preferred color from the partner index (Biased Coloring).
 */
static int ra_hint_color(int v, uint32_t available,
                      const IGraph *g, const PartnerIndex *pidx){
    if (!pidx->start) return -1;

    for (int k = pidx->start[v]; k < pidx->start[v + 1]; k++) {
        int partner = pidx->data[k];
        int pc = g->color[partner];
        if (pc < 0 || (unsigned)pc >= PHYS_ALLOCATABLE) continue;

        // Check if partner's color is available for v
        if ((available >> pc) & 1u) {
            return pc;
        }
    }
    return -1;
}

/**
 * Computes the mask of forbidden physical colors for node @p v.
 */
static inline uint32_t ra_compute_forbidden_colors(const IGraph *g, int v) {
    uint32_t forbidden = g->excl[v];
    for (int k = 0; k < g->adj[v].len; k++) {
        int w = g->adj[v].data[k];
        if (g->color[w] >= 0) {
            forbidden |= (1u << g->color[w]);
        }
    }
    return forbidden;
}


static inline int ra_choose_color(int v, uint32_t available,
                               const IGraph *g,const PartnerIndex *pidx){
    // Priority 1: biased hint from a move-related partner
    int hint = ra_hint_color(v, available, g, pidx);
    if (hint >= 0) return hint;

    // Priority 2: live across CALL — prefer callee-saved registers
    if (g->crossesCall[v]) {
        uint32_t callee = available >> PHYS_CALLER_SAVED_COUNT;
        if (callee) {
            return PHYS_CALLER_SAVED_COUNT + __builtin_ctz(callee);
        }
    }

    // Priority 3: fallback to lowest available color
    if (available) {
        return __builtin_ctz(available);
    }

    return -1; // Needs spill
}


int ra_select_colors(IGraph *g, int *stack, int stackLen,
                     int *spilled, const PartnerList *pl){
    int nSpilled = 0;
    const uint32_t valid_mask = (1u << PHYS_ALLOCATABLE) - 1u;

    // built once for the whole coloring pass instead of scanning pl per node
    PartnerIndex pidx = ra_build_partner_index(pl, g->n);

    for (int si = stackLen - 1; si >= 0; si--) {
        int v = stack[si];
        g->active[v] = 1;

        uint32_t forbidden = ra_compute_forbidden_colors(g, v);
        uint32_t available = (~forbidden) & valid_mask;

        int chosen = ra_choose_color(v, available, g, &pidx);

        if (chosen >= 0) {
            g->color[v] = chosen;
        } else {
            g->color[v] = -2; // Marked as spilled
            spilled[nSpilled++] = v;
        }
    }

    partner_index_free(&pidx);
    return nSpilled;
}