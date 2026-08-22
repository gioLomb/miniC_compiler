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

    // seed buckets: every node whose initial degree is already < k is
    // trivially safe to remove first
    for (int v = 0; v < nextVreg; v++)
        if (g->degree[v] < k)
            bucket_insert(&buckets, v, g->degree[v]);

    while (remaining > 0) {
        int d;
        int chosen = bucket_pop_any_low(&buckets, &d);

        if (chosen < 0) {
            /* no node is safely colorable (all remaining have degree >= k):
             * optimistic spill — pick the active, not-yet-bucketed node
             * with the lowest spillCost/degree ratio as the best spill
             * candidate (cheapest to spill relative to how much it frees up) */
            double best = 1e18;
            for (int v = 0; v < nextVreg; v++) {
                if (!g->active[v] || buckets.inBucket[v]) continue;
                double ratio = g->degree[v] > 0
                               ? (double)g->spillCost[v] / g->degree[v]
                               : 0.0;
                if (ratio < best) { best = ratio; chosen = v; }
            }
            if (chosen < 0) break; // nothing left to process
            // clamp degree to k-1 for bucket bookkeeping consistency below
            d = g->degree[chosen] < k ? g->degree[chosen] : k - 1;
        }

        bucket_remove(&buckets, chosen, d);
        g->active[chosen] = 0;
        remaining--;
        (*outStack)[stackLen++] = chosen;

        // removing 'chosen' lowers the degree of every active neighbour by
        // one; neighbours crossing below k must move into a lower bucket
        for (int idx = 0; idx < g->adj[chosen].len; idx++) {
            int w = g->adj[chosen].data[idx];
            if (w >= nextVreg || !g->active[w]) continue;
            int oldDeg = g->degree[w];
            g->degree[w]--;
            if (oldDeg < k) {
                // already bucketed: move to bucket one lower
                bucket_remove(&buckets, w, oldDeg);
                bucket_insert(&buckets, w, oldDeg - 1);
            } else if (oldDeg == k) {
                // just dropped below k: now guaranteed colorable, insert
                bucket_insert(&buckets, w, k - 1);
            }
        }
    }

    buckets_free(&buckets);
    return stackLen;
}

/* =========================================================================
 * hint_color — look up a preferred color from move-related pairs.
 *
 * Scans all pairs: if v's partner already has a valid color, return it
 * as the hint. Returns -1 if no applicable hint exists.
 * ========================================================================= */
static int hint_color(int v, uint32_t available,
                      const IGraph *g, int nextVreg,
                      const MoveList *ml)
{
    if (!ml || ml->count == 0) return -1;

    for (int i = 0; i < ml->count; i++) {
        int u = ml->pairs[i].u;
        int w = ml->pairs[i].v;
        // determine v's partner in this pair (skip if v is neither side)
        int partner = (u == v) ? w : (w == v) ? u : -1;
        if (partner < 0) continue;

        // fetch the partner's assigned color
        int pc = g->color[partner];
        if (pc < 0) continue;                          // not colored yet
        if ((unsigned)pc >= PHYS_ALLOCATABLE) continue; // not a valid color

        // the hint is only usable if it's still within the available set
        // (i.e. it already satisfies interference + excl + crossesCall)
        if ((available >> pc) & 1u) return pc;
    }
    return -1;
}

/* =========================================================================
 * ra_select_colors — assign colors in reverse simplification order.
 *
 * Biased coloring: if a move-related partner already has an available
 * color, prefer it — eliminates the redundant MOV without touching the
 * graph. Fallback logic is identical to the non-coalescing baseline.
 * ========================================================================= */
int ra_select_colors(IGraph *g, int nextVreg, int *stack, int stackLen,
                     int *spilled, const MoveList *ml)
{
    (void)nextVreg;
    int nSpilled = 0;

    // reinsert nodes in reverse removal order: by the time a node is
    // reinserted, all its neighbours removed *after* it (i.e. processed
    // earlier in this loop) are already colored
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

        /* --- Biased hint: prefer the move-related partner's color --- */
        int hint = hint_color(v, available, g, nextVreg, ml);
        if (hint >= 0) {
            chosen = hint;
        } else if (g->crossesCall[v]) {
            // vreg live across a CALL: prefer callee-saved colors first to
            // avoid caller-saved clobbering and reduce push/pop overhead
            uint32_t callee = available >> PHYS_CALLER_SAVED_COUNT;
            if (callee)
                chosen = PHYS_CALLER_SAVED_COUNT + __builtin_ctz(callee);
            else if (available)
                chosen = __builtin_ctz(available);
        } else {
            // no special preference: pick the lowest available color
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