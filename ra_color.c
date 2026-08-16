#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ra_color.h"
#include "instr_selector.h"   /* PHYS_ALLOCATABLE, PHYS_CALLER_SAVED_COUNT, PHYS_RBX..R15 */


/* =========================================================================
 * ra_simplify — Briggs-optimistic simplification.
 *
 * Nodi con grado < k: rimossi subito (colorabili con certezza).
 * Nodi con grado ≥ k: potenziali spill; si sceglie quello con rapporto
 *   spillCost/degree minimo (Chaitin heuristic).
 *
 * Restituisce lunghezza dello stack; *outStack è malloc del chiamante.
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

        if (chosen < 0) {
            /* nessun nodo colorabile con certezza: spill ottimistico */
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
        }

        bucket_remove(&buckets, chosen, d);
        g->active[chosen] = 0;
        remaining--;
        (*outStack)[stackLen++] = chosen;

        /* aggiorna gradi dei vicini ancora attivi */
        for (int idx = 0; idx < g->adj[chosen].len; idx++) {
            int w = g->adj[chosen].data[idx];
            if (w >= nextVreg || !g->active[w]) continue;
            int oldDeg = g->degree[w];
            g->degree[w]--;
            if (oldDeg < k) {
                bucket_remove(&buckets, w, oldDeg);
                bucket_insert(&buckets, w, oldDeg - 1);
            } else if (oldDeg == k) {
                /* scende sotto la soglia: ora colorabile con certezza */
                bucket_insert(&buckets, w, k - 1);
            }
        }
    }

    buckets_free(&buckets);
    return stackLen;
}

/* =========================================================================
 * ra_select_colors — assegna colori in ordine inverso allo stack.
 *
 * Strategia: usa __builtin_ctz su bitmask available per O(1).
 * Preferisce callee-saved per live-across-call (meno push/pop al prologo).
 * Nodi senza colore → spilled[]; restituisce nSpilled.
 * ========================================================================= */
int ra_select_colors(IGraph *g, int nextVreg, int *stack, int stackLen,
                     int *spilled)
{
    (void)nextVreg;
    int nSpilled = 0;

    for (int si = stackLen - 1; si >= 0; si--) {
        int v = stack[si];
        g->active[v] = 1;

        /* forbidden: registri fisici usati da vicini già colorati + exclusions */
        uint32_t forbidden = g->excl[v];
        for (int k = 0; k < g->adj[v].len; k++) {
            int w = g->adj[v].data[k];
            if (g->color[w] >= 0)
                forbidden |= (1u << g->color[w]);
        }

        const uint32_t valid_mask = (1u << PHYS_ALLOCATABLE) - 1u;
        uint32_t available = (~forbidden) & valid_mask;

        int chosen = -1;
        if (g->crossesCall[v]) {
            /* preferisci callee-saved */
            uint32_t callee = available >> PHYS_CALLER_SAVED_COUNT;
            if (callee)
                chosen = PHYS_CALLER_SAVED_COUNT + __builtin_ctz(callee);
            else if (available)
                chosen = __builtin_ctz(available);
        } else {
            if (available)
                chosen = __builtin_ctz(available);
        }

        if (chosen >= 0)
            g->color[v] = chosen;
        else {
            g->color[v]         = -2;   /* marcato spill */
            spilled[nSpilled++] = v;
        }
    }

    return nSpilled;
}