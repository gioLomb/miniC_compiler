#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../arena.h"
#include "../interference.h"
#include "../ra_color.h"
#include "../ra_coalesce.h"

/* */
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
    /* */
    g.isReloadTemp = arena_alloc(arena, (size_t)n * sizeof(char));
    memset(g.isReloadTemp, 0, (size_t)n * sizeof(char));

    for (int i = 0; i < n; i++) {
        int_vector_init(&g.adj[i],IG_ADJ_INITIAL_CAPACITY);
        g.degree[i] = 0;
        g.color[i]  = COLOR_NONE;
        g.active[i] = true;
        g.excl[i]   = 0;
        g.spillCost[i] = 1;      /* */
        g.crossesCall[i] = 0;
    }

    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            long idx = (long)j * (j - 1) / 2 + i;   /* */
            g.matrix[idx >> 6] |= 1ULL << (idx & 63);
            int_vector_push(&g.adj[i], j);
            int_vector_push(&g.adj[j], i);
            g.degree[i]++; g.degree[j]++;
        }
    }
    return g;
}

/* */
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
    g.isReloadTemp = arena_alloc(arena, (size_t)n * sizeof(char)); /* */
    memset(g.isReloadTemp, 0, (size_t)n * sizeof(char));
    for (int i = 0; i < n; i++) {
        int_vector_init(&g.adj[i],IG_ADJ_INITIAL_CAPACITY);
        g.degree[i] = 0; g.color[i] = COLOR_NONE; g.active[i] = true;
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

int main(void) {

    /* PASS 1 */
    {
        int n = PHYS_ALLOCATABLE + 1;   /* */
        Arena *arena = arena_create(0);
        IGraph g = build_clique(n, arena);

        int *stack;
        int stackLen = ra_simplify(&g, n, PHYS_ALLOCATABLE, &stack);
        assert(stackLen == n && "ogni nodo must be rimosso e impilato una volta");

        int *spilled = malloc((size_t)n * sizeof(int));
        /* */
        int nSpilled = ra_select_colors(&g, stack, stackLen, PHYS_ALLOCATABLE, PHYS_CALLER_SAVED_COUNT, spilled, NULL);

        /* */
        assert(nSpilled == n - PHYS_ALLOCATABLE);

        /* */
        int seenColor[PHYS_ALLOCATABLE]; memset(seenColor, 0, sizeof(seenColor));
        int coloredCount = 0;
        for (int v = 0; v < n; v++) {
            if (g.color[v] == COLOR_SPILLED) continue;
            assert(g.color[v] >= 0 && g.color[v] < PHYS_ALLOCATABLE);
            assert(!seenColor[g.color[v]] && "two nodes of a clique cannot share a color");
            seenColor[g.color[v]] = 1;
            coloredCount++;
        }
        assert(coloredCount == PHYS_ALLOCATABLE);

        printf("PASS 1 ok: clique of %d nodes -> %d spills, %d distinct colors "
               "(regression Bug2, nessuna corruzione bucket).\n",
               n, nSpilled, coloredCount);

        free(stack); free(spilled);
        arena_destroy(arena);
    }

    /* PASS 2 */
    {
        int n = PHYS_ALLOCATABLE;
        Arena *arena = arena_create(0);
        IGraph g = build_clique(n, arena);

        int *stack;
        int stackLen = ra_simplify(&g, n, PHYS_ALLOCATABLE, &stack);
        assert(stackLen == n);

        int *spilled = malloc((size_t)n * sizeof(int));
        int nSpilled = ra_select_colors(&g, stack, stackLen, PHYS_ALLOCATABLE, PHYS_CALLER_SAVED_COUNT, spilled, NULL);
        assert(nSpilled == 0 && "una clique of esattamente k nodi must be colorabile");

        int seen[PHYS_ALLOCATABLE]; memset(seen, 0, sizeof(seen));
        for (int v = 0; v < n; v++) {
            assert(g.color[v] >= 0 && g.color[v] < PHYS_ALLOCATABLE);
            assert(!seen[g.color[v]]);
            seen[g.color[v]] = 1;
        }
        printf("PASS 2 ok: clique of k=%d nodi colored without spill.\n", n);
        free(stack); free(spilled);
        arena_destroy(arena);
    }

    /* PASS 3 */
    {
        int n = 50;
        Arena *arena = arena_create(0);
        IGraph g = build_path(n, arena);

        int *stack;
        int stackLen = ra_simplify(&g, n, PHYS_ALLOCATABLE, &stack);
        assert(stackLen == n);

        int *spilled = malloc((size_t)n * sizeof(int));
        int nSpilled = ra_select_colors(&g, stack, stackLen, PHYS_ALLOCATABLE, PHYS_CALLER_SAVED_COUNT, spilled, NULL);
        assert(nSpilled == 0 && "un path graph (grado<=2) e' sempre 2-colorabile, a maggior ragione con k=14");

        for (int i = 0; i + 1 < n; i++)
            assert(g.color[i] != g.color[i + 1] &&
                   "nodi adiacenti nel path non possono condividere colore");

        printf("PASS 3 ok: path graph of %d nodi colored without spill, no adjacent conflicts.\n", n);
        free(stack); free(spilled);
        arena_destroy(arena);
    }

    /* PASS 4 */
    {
        int n = 2;
        Arena *arena = arena_create(0);
        IGraph g;
        g.n = n;
        size_t words = 1;
        g.matrix = arena_alloc(arena, words * sizeof(uint64_t));
        memset(g.matrix, 0, words * sizeof(uint64_t)); /* */
        g.adj = arena_alloc(arena, (size_t)n * sizeof(AdjList));
        for (int i = 0; i < n; i++) int_vector_init(&g.adj[i],IG_ADJ_INITIAL_CAPACITY);
        g.degree      = arena_alloc(arena, (size_t)n * sizeof(int));
        g.color       = arena_alloc(arena, (size_t)n * sizeof(int));
        g.active      = arena_alloc(arena, (size_t)n * sizeof(bool));
        g.excl        = arena_alloc(arena, (size_t)n * sizeof(uint32_t));
        g.spillCost   = arena_alloc(arena, (size_t)n * sizeof(int));
        g.crossesCall = arena_alloc(arena, (size_t)n * sizeof(char));
        g.isReloadTemp = arena_alloc(arena, (size_t)n * sizeof(char));
        memset(g.degree, 0, (size_t)n * sizeof(int));
        memset(g.excl, 0, (size_t)n * sizeof(uint32_t));
        memset(g.spillCost, 0, (size_t)n * sizeof(int));
        memset(g.crossesCall, 0, (size_t)n * sizeof(char));
        memset(g.isReloadTemp, 0, (size_t)n * sizeof(char));
        g.active[0] = true; g.active[1] = true;
        g.color[0] = COLOR_NONE;
        g.color[1] = 5;          /* */

        int stack[1] = { 0 };    /* */
        int spilled[1];
        PartnerList pl = { NULL, 0, 0 };
        pl.pairs = malloc(sizeof *pl.pairs);
        pl.pairs[0] = (PartnerPair){ .vregId = 0, .partnerId = 1 };
        pl.count = 1; pl.cap = 1;

        int nSpilled = ra_select_colors(&g, stack, 1, PHYS_ALLOCATABLE, PHYS_CALLER_SAVED_COUNT, spilled, &pl);
        assert(nSpilled == 0);
        assert(g.color[0] == 5 && "il biased coloring must reuse il colore 5 del partner");

        printf("PASS 4 ok: biased coloring reuses color 5 of the partner non interferente.\n");
        free(pl.pairs);
        arena_destroy(arena);
    }

    /* PASS 5 */
    {
        int n = PHYS_ALLOCATABLE + 1; /* */
        Arena *arena = arena_create(0);
        IGraph g = build_clique(n, arena);

        /* */
        g.isReloadTemp[0] = 1;
        g.spillCost[0]    = 1;
        /* */
        for (int v = 1; v < n; v++) g.spillCost[v] = 10000;

        int *stack;
        int stackLen = ra_simplify(&g, n, PHYS_ALLOCATABLE, &stack);
        assert(stackLen == n);

        /* */
        assert(stack[0] != 0 &&
               "il primo spill ottimistico must not mai cadere sul reload-temp "
               "quando esiste un candidato reale, indipendentemente dal ratio");

        printf("PASS 5 ok: ra_simplify prefers a real candidate over the reload-temp "
               "anche con ratio spillCost/degree peggiore.\n");
        free(stack);
        arena_destroy(arena);
    }

    /* PASS 6 */
    {
        int n = 3; /* */
        Arena *arena = arena_create(0);
        IGraph g;
        g.n = n;
        g.matrix = arena_alloc(arena, sizeof(uint64_t));
        memset(g.matrix, 0, sizeof(uint64_t)); /* */
        g.adj = arena_alloc(arena, (size_t)n * sizeof(AdjList));
        for (int i = 0; i < n; i++) int_vector_init(&g.adj[i],IG_ADJ_INITIAL_CAPACITY);
        g.degree       = arena_alloc(arena, (size_t)n * sizeof(int));
        g.color        = arena_alloc(arena, (size_t)n * sizeof(int));
        g.active       = arena_alloc(arena, (size_t)n * sizeof(bool));
        g.excl         = arena_alloc(arena, (size_t)n * sizeof(uint32_t));
        g.spillCost    = arena_alloc(arena, (size_t)n * sizeof(int));
        g.crossesCall  = arena_alloc(arena, (size_t)n * sizeof(char));
        g.isReloadTemp = arena_alloc(arena, (size_t)n * sizeof(char));
        memset(g.degree, 0, (size_t)n * sizeof(int));
        memset(g.excl, 0, (size_t)n * sizeof(uint32_t));
        memset(g.spillCost, 0, (size_t)n * sizeof(int));
        memset(g.crossesCall, 0, (size_t)n * sizeof(char));
        memset(g.isReloadTemp, 0, (size_t)n * sizeof(char));
        g.active[0] = g.active[1] = g.active[2] = true;

        g.color[0] = COLOR_NONE;   /* */
        g.color[1] = 0;    /* */
        g.color[2] = 3;    /* */
        g.excl[0]  = (1u << 0); /* */

        int stack[1] = { 0 };
        int spilled[1];
        PartnerList pl = { NULL, 0, 0 };
        pl.pairs = malloc(2 * sizeof *pl.pairs);
        pl.pairs[0] = (PartnerPair){ .vregId = 0, .partnerId = 1 }; /* */
        pl.pairs[1] = (PartnerPair){ .vregId = 0, .partnerId = 2 }; /* */
        pl.count = 2; pl.cap = 2;

        int nSpilled = ra_select_colors(&g, stack, 1, PHYS_ALLOCATABLE, PHYS_CALLER_SAVED_COUNT, spilled, &pl);
        assert(nSpilled == 0);
        assert(g.color[0] == 3 &&
               "hint del primo partner (colore escluso) must be scartato; "
               "il secondo partner (colore 3, disponibile) va usato");

        printf("PASS 6 ok: hint_color discards a partner with unavailable color "
               "e usa correctmente il successivo.\n");
        free(pl.pairs);
        arena_destroy(arena);
    }

    printf("\nAll ra_color tests passed.\n");
    return 0;
}