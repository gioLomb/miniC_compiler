#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ra_color.h"
#include "ra_coalesce.h"
#include "instr_selector.h"   /* PHYS_ALLOCATABLE, PHYS_CALLER_SAVED_COUNT, PHYS_RBX..R15 */


/* =========================================================================
 * ra_simplify — Briggs-optimistic simplification.
 * (invariato rispetto alla versione originale)
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
 * hint_color — cerca colore preferito da coppie move-related.
 *
 * Scansiona le coppie: se il partner di `v` ha già un colore valido
 * (dentro `available`), lo restituisce come hint.
 * Ritorna -1 se nessun hint applicabile.
 * ========================================================================= */
static int hint_color(int v, uint32_t available,
                      const IGraph *g, int nextVreg,
                      const MoveList *ml)
{
    if (!ml || ml->count == 0) return -1;

    for (int i = 0; i < ml->count; i++) {
        int u = ml->pairs[i].u;
        int w = ml->pairs[i].v;
        /* partner di v è u o w */
        int partner = (u == v) ? w : (w == v) ? u : -1;
        if (partner < 0) continue;

        /* Ottieni colore del partner */
        int pc = g->color[partner];
        if (pc < 0) continue;                          /* non ancora colorato */
        if ((unsigned)pc >= PHYS_ALLOCATABLE) continue; /* colore non valido  */

        /* Controlla che il colore del partner sia in `available` */
        if ((available >> pc) & 1u) return pc;
    }
    return -1;
}

/* =========================================================================
 * ra_select_colors — assegna colori in ordine inverso allo stack.
 *
 * Biased coloring: se un partner move-related ha già un colore disponibile,
 * lo si preferisce → elimina il MOV ridondante senza toccare il grafo.
 * Fallback identico alla versione originale.
 * ========================================================================= */
int ra_select_colors(IGraph *g, int nextVreg, int *stack, int stackLen,
                     int *spilled, const MoveList *ml)
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

        /* --- Biased hint: preferisci colore del partner move-related --- */
        int hint = hint_color(v, available, g, nextVreg, ml);
        if (hint >= 0) {
            chosen = hint;
        } else if (g->crossesCall[v]) {
            /* preferisci callee-saved per vreg live-across-call */
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