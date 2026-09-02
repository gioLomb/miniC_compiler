#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <float.h>
#include "ra_color.h"
#include "ra_coalesce.h"
#include "instr_selector.h"   /* PHYS_ALLOCATABLE, PHYS_CALLER_SAVED_COUNT, PHYS_RBX..R15 */

/* =========================================================================
 * Helper Functions — Simplification Phase (Refactoring: Extract Function)
 * ========================================================================= */

/**
 * Scans active, non-bucketed nodes to find the best optimistic spill candidate.
 * Strictly prefers real IR variables over reload temporaries.
 */
static int select_spill_candidate(const IGraph *g, int nextVreg, const Buckets *buckets) {
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
static inline void update_neighbor_degrees(IGraph *g, int chosen, int nextVreg, int k, Buckets *buckets) {
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
            chosen = select_spill_candidate(g, nextVreg, &buckets);
            if (chosen < 0) break; // Impossible to proceed

            degree = (g->degree[chosen] < k) ? g->degree[chosen] : (k - 1);
        } else {
            bucket_remove(&buckets, chosen, degree);
        }

        g->active[chosen] = 0;
        remaining--;
        (*outStack)[stackLen++] = chosen;

        update_neighbor_degrees(g, chosen, nextVreg, k, &buckets);
    }

    buckets_free();
    return stackLen;
}

/* =========================================================================
 * Helper Functions — Selection Phase (Refactoring: Extract Function)
 * ========================================================================= */

/**
 * Returns the partner ID for node @p v in the pair, or -1 if @p v is not part of it.
 */
static inline int get_partner_id(const PartnerPair *pair, int v) {
    if (pair->u == v) return pair->v;
    if (pair->v == v) return pair->u;
    return -1;
}

/**
 * Look up a preferred color from the partner list (Biased Coloring).
 */
static int hint_color(int v, uint32_t available,
                      const IGraph *g,const PartnerList *pl){
    if (!pl || pl->count == 0) return -1;

    for (int i = 0; i < pl->count; i++) {
        int partner = get_partner_id(&pl->pairs[i], v);
        if (partner < 0) continue;

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
static inline uint32_t compute_forbidden_colors(const IGraph *g, int v) {
    uint32_t forbidden = g->excl[v];
    for (int k = 0; k < g->adj[v].len; k++) {
        int w = g->adj[v].data[k];
        if (g->color[w] >= 0) {
            forbidden |= (1u << g->color[w]);
        }
    }
    return forbidden;
}


static inline int choose_color(int v, uint32_t available,
                               const IGraph *g,const PartnerList *pl){
    // Priority 1: biased hint from a move-related partner
    int hint = hint_color(v, available, g, pl);
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

    for (int si = stackLen - 1; si >= 0; si--) {
        int v = stack[si];
        g->active[v] = 1;

        uint32_t forbidden = compute_forbidden_colors(g, v);
        uint32_t available = (~forbidden) & valid_mask;

        int chosen = choose_color(v, available, g, pl);

        if (chosen >= 0) {
            g->color[v] = chosen;
        } else {
            g->color[v] = -2; // Marked as spilled
            spilled[nSpilled++] = v;
        }
    }

    return nSpilled;
}