/**
 * @file test_ra_color.c
 * @brief Unit test per ra_color.c (Simplify + Select, Chaitin-Briggs).
 *
 * Il focus principale e' la regressione del Bug 2: quando nessun nodo ha
 * grado < k (PHYS_ALLOCATABLE), ra_simplify() sceglie un candidato di spill
 * ottimistico che non e' MAI stato inserito in un bucket. La build
 * precedente chiamava comunque bucket_remove() su quel nodo, leggendo
 * prev[]/next[] non inizializzati (arena_alloc non azzera) e corrompendo
 * una lista concatenata arbitraria. Una cricca di k+1 nodi forza questo
 * percorso al primissimo passo (nessun nodo iniziale ha grado < k).
 *
 * Compilare con -fsanitize=address,undefined rende visibile la corruzione
 * anche quando non produce un crash immediato.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../arena.h"
#include "../interference.h"
#include "../ra_color.h"
#include "../ra_coalesce.h"

/**
 * @brief Costruisce una IGraph "cricca" di @p n nodi (tutti interferiscono
 *        a coppie), senza nodi fisici pre-colorati, per isolare la logica
 *        di Simplify/Select dalla costruzione via ig_build().
 */
static IGraph build_clique(int n, Arena *arena) {
    IGraph g; g.n = n;
    long nbits = (long)n * (n - 1) / 2;
    size_t words = (size_t)((nbits + 63) / 64 + 1);
    g.matrix = arena_alloc(arena, words * sizeof(uint64_t));
    memset(g.matrix, 0, words * sizeof(uint64_t));

    g.adj = arena_alloc(arena, (size_t)n * sizeof(AdjList));
    g.degree      = arena_alloc(arena, (size_t)n * sizeof(int));
    g.color       = arena_alloc(arena, (size_t)n * sizeof(int));
    g.active      = arena_alloc(arena, (size_t)n * sizeof(bool));
    g.excl        = arena_alloc(arena, (size_t)n * sizeof(uint32_t));
    g.spillCost   = arena_alloc(arena, (size_t)n * sizeof(int));
    g.crossesCall = arena_alloc(arena, (size_t)n * sizeof(char));

    for (int i = 0; i < n; i++) {
        int_vector_init(&g.adj[i]);
        g.degree[i] = 0;
        g.color[i]  = -1;
        g.active[i] = true;
        g.excl[i]   = 0;
        g.spillCost[i] = 1;      /* tutti costo uguale: lo spill e' arbitrario tra pari */
        g.crossesCall[i] = 0;
    }

    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            long idx = (long)j * (j - 1) / 2 + i;   /* j>i sempre qui */
            g.matrix[idx >> 6] |= 1ULL << (idx & 63);
            int_vector_push(&g.adj[i], j);
            int_vector_push(&g.adj[j], i);
            g.degree[i]++; g.degree[j]++;
        }
    }
    return g;
}

/** Costruisce una IGraph "path" (catena i - i+1), sempre grado <= 2. */
static IGraph build_path(int n, Arena *arena) {
    IGraph g; g.n = n;
    long nbits = (long)n * (n - 1) / 2;
    size_t words = (size_t)((nbits + 63) / 64 + 1);
    g.matrix = arena_alloc(arena, words * sizeof(uint64_t));
    memset(g.matrix, 0, words * sizeof(uint64_t));
    g.adj = arena_alloc(arena, (size_t)n * sizeof(AdjList));
    g.degree      = arena_alloc(arena, (size_t)n * sizeof(int));
    g.color       = arena_alloc(arena, (size_t)n * sizeof(int));
    g.active      = arena_alloc(arena, (size_t)n * sizeof(bool));
    g.excl        = arena_alloc(arena, (size_t)n * sizeof(uint32_t));
    g.spillCost   = arena_alloc(arena, (size_t)n * sizeof(int));
    g.crossesCall = arena_alloc(arena, (size_t)n * sizeof(char));
    for (int i = 0; i < n; i++) {
        int_vector_init(&g.adj[i]);
        g.degree[i] = 0; g.color[i] = -1; g.active[i] = true;
        g.excl[i] = 0; g.spillCost[i] = 1; g.crossesCall[i] = 0;
    }
    for (int i = 0; i + 1 < n; i++) {
        int j = i + 1;
        long idx = (long)j * (j - 1) / 2 + i;
        g.matrix[idx >> 6] |= 1ULL << (idx & 63);
        int_vector_push(&g.adj[i], j);
        int_vector_push(&g.adj[j], i);
        g.degree[i]++; g.degree[j]++;
    }
    return g;
}

static int edge(const IGraph *g, int i, int j) {
    long idx;
    if (i < j) { int t = i; i = j; j = t; }
    idx = (long)i * (i - 1) / 2 + j;
    return (int)((g->matrix[idx >> 6] >> (idx & 63)) & 1ULL);
}

int main(void) {

    /* ================================================================
     * PASS 1 (regressione Bug 2): cricca di k+1 = 15 nodi.
     * Nessun nodo parte con grado < k=14: il PRIMISSIMO bucket_pop_any_low
     * ritorna -1, forzando subito il ramo di spill ottimistico su un nodo
     * mai bucketizzato -- esattamente il percorso che corrompeva la
     * memoria prima del fix.
     * ================================================================ */
    {
        int n = PHYS_ALLOCATABLE + 1;   /* 15 */
        Arena *arena = arena_create(0);
        IGraph g = build_clique(n, arena);

        int *stack;
        int stackLen = ra_simplify(&g, n, &stack);
        assert(stackLen == n && "ogni nodo deve essere rimosso e impilato una volta");

        int *spilled = malloc((size_t)n * sizeof(int));
        int nSpilled = ra_select_colors(&g, n, stack, stackLen, spilled, NULL);

        /* Una cricca di n nodi richiede esattamente n colori: con k
         * disponibili, esattamente (n-k) nodi devono spillare. */
        assert(nSpilled == n - PHYS_ALLOCATABLE);

        /* Tutti i nodi NON spillati devono avere colori a due a due
         * distinti (sono tutti a due a due interferenti). */
        int seenColor[PHYS_ALLOCATABLE]; memset(seenColor, 0, sizeof(seenColor));
        int coloredCount = 0;
        for (int v = 0; v < n; v++) {
            if (g.color[v] == -2) continue; /* spilled */
            assert(g.color[v] >= 0 && g.color[v] < PHYS_ALLOCATABLE);
            assert(!seenColor[g.color[v]] && "due nodi di una cricca non possono condividere colore");
            seenColor[g.color[v]] = 1;
            coloredCount++;
        }
        assert(coloredCount == PHYS_ALLOCATABLE);

        printf("PASS 1 ok: cricca di %d nodi -> %d spill, %d colorati distinti "
               "(regressione Bug2, nessuna corruzione bucket).\n",
               n, nSpilled, coloredCount);

        free(stack); free(spilled);
        arena_destroy(arena);
    }

    /* ================================================================
     * PASS 2: cricca esatta di k nodi -> zero spill, colorazione = bijezione
     * su [0,k).
     * ================================================================ */
    {
        int n = PHYS_ALLOCATABLE;
        Arena *arena = arena_create(0);
        IGraph g = build_clique(n, arena);

        int *stack;
        int stackLen = ra_simplify(&g, n, &stack);
        assert(stackLen == n);

        int *spilled = malloc((size_t)n * sizeof(int));
        int nSpilled = ra_select_colors(&g, n, stack, stackLen, spilled, NULL);
        assert(nSpilled == 0 && "una cricca di esattamente k nodi deve essere colorabile");

        int seen[PHYS_ALLOCATABLE]; memset(seen, 0, sizeof(seen));
        for (int v = 0; v < n; v++) {
            assert(g.color[v] >= 0 && g.color[v] < PHYS_ALLOCATABLE);
            assert(!seen[g.color[v]]);
            seen[g.color[v]] = 1;
        }
        printf("PASS 2 ok: cricca di k=%d nodi colorata senza spill.\n", n);
        free(stack); free(spilled);
        arena_destroy(arena);
    }

    /* ================================================================
     * PASS 3: path graph di 50 nodi (grado <= 2, sempre < k): tutti i nodi
     * passano dal percorso normale (bucketizzato) di ra_simplify, zero
     * spill attesi, e nessuna coppia adiacente condivide colore.
     * ================================================================ */
    {
        int n = 50;
        Arena *arena = arena_create(0);
        IGraph g = build_path(n, arena);

        int *stack;
        int stackLen = ra_simplify(&g, n, &stack);
        assert(stackLen == n);

        int *spilled = malloc((size_t)n * sizeof(int));
        int nSpilled = ra_select_colors(&g, n, stack, stackLen, spilled, NULL);
        assert(nSpilled == 0 && "un path graph (grado<=2) e' sempre 2-colorabile, a maggior ragione con k=14");

        for (int i = 0; i + 1 < n; i++)
            assert(g.color[i] != g.color[i + 1] &&
                   "nodi adiacenti nel path non possono condividere colore");

        printf("PASS 3 ok: path graph di %d nodi colorato senza spill, nessun conflitto adiacente.\n", n);
        free(stack); free(spilled);
        arena_destroy(arena);
    }

    /* ================================================================
     * PASS 4: biased coloring — se un partner (gia' colorato) offre un
     * colore ancora disponibile, ra_select_colors deve preferirlo.
     * v0 e v1 non interferiscono; v1 e' pre-colorato a 5 (simula un
     * fisico/partner gia' fissato) e non passa da Simplify (non e' in
     * stack). ra_select_colors su v0 con hint {0,1} deve assegnargli 5.
     * ================================================================ */
    {
        int n = 2;
        Arena *arena = arena_create(0);
        IGraph g;
        g.n = n;
        size_t words = 1;
        g.matrix = arena_alloc(arena, words * sizeof(uint64_t));
        memset(g.matrix, 0, words * sizeof(uint64_t)); /* nessun arco: v0,v1 non interferiscono */
        g.adj = arena_alloc(arena, (size_t)n * sizeof(AdjList));
        for (int i = 0; i < n; i++) int_vector_init(&g.adj[i]);
        g.degree      = arena_alloc(arena, (size_t)n * sizeof(int));
        g.color       = arena_alloc(arena, (size_t)n * sizeof(int));
        g.active      = arena_alloc(arena, (size_t)n * sizeof(bool));
        g.excl        = arena_alloc(arena, (size_t)n * sizeof(uint32_t));
        g.spillCost   = arena_alloc(arena, (size_t)n * sizeof(int));
        g.crossesCall = arena_alloc(arena, (size_t)n * sizeof(char));
        memset(g.degree, 0, (size_t)n * sizeof(int));
        memset(g.excl, 0, (size_t)n * sizeof(uint32_t));
        memset(g.spillCost, 0, (size_t)n * sizeof(int));
        memset(g.crossesCall, 0, (size_t)n * sizeof(char));
        g.active[0] = true; g.active[1] = true;
        g.color[0] = -1;
        g.color[1] = 5;          /* partner "gia' colorato" (fuori da Simplify) */

        int stack[1] = { 0 };    /* solo v0 va colorato */
        int spilled[1];
        PartnerList pl = { NULL, 0, 0 };
        pl.pairs = malloc(sizeof *pl.pairs);
        pl.pairs[0] = (PartnerPair){ .u = 0, .v = 1 };
        pl.count = 1; pl.cap = 1;

        int nSpilled = ra_select_colors(&g, n, stack, 1, spilled, &pl);
        assert(nSpilled == 0);
        assert(g.color[0] == 5 && "il biased coloring deve riusare il colore del partner");

        printf("PASS 4 ok: biased coloring riusa il colore 5 del partner non interferente.\n");
        free(pl.pairs);
        arena_destroy(arena);
    }

    printf("\nTutti i test ra_color sono passati.\n");
    return 0;
}