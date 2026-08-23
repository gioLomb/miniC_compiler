#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ra_color.h"
#include "ra_coalesce.h"
#include "instr_selector.h"   /* PHYS_ALLOCATABLE, PHYS_CALLER_SAVED_COUNT, PHYS_RBX..R15 */


/* =========================================================================
 * ra_simplify — Briggs-optimistic simplification.
 * ========================================================================= */
int ra_simplify(IGraph *g, int nextVreg, int **outStack)
{
    *outStack = malloc((size_t)(nextVreg > 0 ? nextVreg : 1) * sizeof(int));
    int stackLen  = 0;
    int k         = PHYS_ALLOCATABLE;
    Buckets buckets = buckets_create(nextVreg, k);
    int remaining   = nextVreg;

    for (int v = 0; v < nextVreg; v++)
        if (g->degree[v] < k)
            bucket_insert(&buckets, v, g->degree[v]);

    while (remaining > 0) {
        int d;
        int chosen = bucket_pop_any_low(&buckets, &d);
        int fromBucket = (chosen >= 0);   /* FIX: traccia provenienza */

        if (chosen < 0) {
            double best = 1e18;
            for (int v = 0; v < nextVreg; v++) {
                if (!g->active[v] || buckets.inBucket[v]) continue;
                double ratio = g->degree[v] > 0
                               ? (double)g->spillCost[v] / g->degree[v]
                               : 0.0;
                if (ratio < best) { best = ratio; chosen = v; }
            }
            if (chosen < 0) break;
            d = g->degree[chosen] < k ? g->degree[chosen] : k - 1;
            /* fromBucket resta 0: questo nodo non e' mai stato bucketizzato */
        }

        /* FIX: bucket_remove() SOLO se chosen viene realmente da un bucket.
         * Il candidato spill scelto sopra non e' mai stato inserito. */
        if (fromBucket)
            bucket_remove(&buckets, chosen, d);

        g->active[chosen] = 0;
        remaining--;
        (*outStack)[stackLen++] = chosen;

        for (int idx = 0; idx < g->adj[chosen].len; idx++) {
            int w = g->adj[chosen].data[idx];
            if (w >= nextVreg || !g->active[w]) continue;
            int oldDeg = g->degree[w];
            g->degree[w]--;
            if (oldDeg < k) {
                bucket_remove(&buckets, w, oldDeg);
                bucket_insert(&buckets, w, oldDeg - 1);
            } else if (oldDeg == k) {
                bucket_insert(&buckets, w, k - 1);
            }
        }
    }

    buckets_free(&buckets);
    return stackLen;
}
/* =========================================================================
 * hint_color — look up a preferred color from the partner list.
 *
 * Scans all pairs in @p pl: if the current node @p v has a partner that
 * already holds a valid color and that color is still within @p available,
 * return it as the biased-coloring hint.  Returns -1 if no applicable hint.
 * ========================================================================= */
static int hint_color(int v, uint32_t available,
                      const IGraph *g, int nextVreg,
                      const PartnerList *pl)
{
    if (!pl || pl->count == 0) return -1;

    for (int i = 0; i < pl->count; i++) {
        int u = pl->pairs[i].u;
        int w = pl->pairs[i].v;
        // determine v's partner in this pair; skip if v is neither side
        int partner = (u == v) ? w : (w == v) ? u : -1;
        if (partner < 0) continue;

        // fetch the partner's already-assigned color
        int pc = g->color[partner];
        if (pc < 0) continue;                           // not colored yet
        if ((unsigned)pc >= PHYS_ALLOCATABLE) continue; // out of valid range

        // the hint is only usable when the partner's color is still available
        // (already satisfies interference + excl + crossesCall for this node)
        if ((available >> pc) & 1u) return pc;
    }
    return -1;
}

/* =========================================================================
 * ra_select_colors — assign colors in reverse simplification order.
 *
 * Biased coloring: if a partner already has an available color, prefer it
 * to eliminate the redundant MOV without touching the graph.  Fallback
 * logic (callee-saved preference, lowest-color) is identical to the
 * non-coalescing baseline.
 * ========================================================================= */
int ra_select_colors(IGraph *g, int nextVreg, int *stack, int stackLen,
                     int *spilled, const PartnerList *pl)
{
    (void)nextVreg;
    int nSpilled = 0;

    // reinsert nodes in reverse removal order: by the time a node is
    // reinserted, all its neighbours removed *after* it (processed earlier
    // in this loop) are already colored
    for (int si = stackLen - 1; si >= 0; si--) {
        int v = stack[si];
        g->active[v] = 1;

        // forbidden colors: this node's own exclusions plus every color
        // already taken by an active, colored neighbour
        uint32_t forbidden = g->excl[v];
        for (int k = 0; k < g->adj[v].len; k++) {
            int w = g->adj[v].data[k];
            if (g->color[w] >= 0)
                forbidden |= (1u << g->color[w]);
        }

        const uint32_t valid_mask = (1u << PHYS_ALLOCATABLE) - 1u;
        uint32_t available = (~forbidden) & valid_mask;

        int chosen = -1;

        /* Priority 1: biased hint from a move-related partner */
        int hint = hint_color(v, available, g, nextVreg, pl);
        if (hint >= 0) {
            chosen = hint;
        } else if (g->crossesCall[v]) {
            /* Priority 2: vreg live across a CALL — prefer callee-saved colors
             * to avoid caller-saved clobbering and reduce push/pop overhead */
            uint32_t callee = available >> PHYS_CALLER_SAVED_COUNT;
            if (callee)
                chosen = PHYS_CALLER_SAVED_COUNT + __builtin_ctz(callee);
            else if (available)
                chosen = __builtin_ctz(available);
        } else {
            /* Priority 3: no special preference — lowest available color */
            if (available)
                chosen = __builtin_ctz(available);
        }

        if (chosen >= 0)
            g->color[v] = chosen;
        else {
            g->color[v]         = -2;   // marked as spilled: no color fits
            spilled[nSpilled++] = v;
        }
    }

    return nSpilled;
}