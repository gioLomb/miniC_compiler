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
 *
 * ---------------------------------------------------------------------
 * FIX applicato in questa versione: build_clique()/build_path() NON
 * allocavano ne' azzeravano g.isReloadTemp. ra_simplify() (ra_color.c)
 * legge pero' g->isReloadTemp[v] per ogni nodo attivo non bucketizzato nel
 * ramo di spill ottimistico — con isReloadTemp puntatore non inizializzato
 * (IGraph e' una variabile locale non zero-init, "IGraph g; g.n = n;")
 * questo e' un accesso a memoria indefinita, non solo un valore sbagliato:
 * il crash puo' non manifestarsi in build normale (garbage che capita a
 * essere leggibile) ma AddressSanitizer/Valgrind lo segnalano sempre.
 * Esempio concreto: PASS 1 (cricca di 15 nodi, k=14) entra SUBITO nel ramo
 * ottimistico al primo bucket_pop_any_low() (nessun nodo ha grado<k), e per
 * ognuno degli score valuta `if (g->isReloadTemp[v])` — con isReloadTemp
 * non allocato questo legge un puntatore casuale della porzione di stack
 * riusata da chiamate precedenti. Fix: allocare+azzerare isReloadTemp come
 * gli altri campi paralleli.
 * ---------------------------------------------------------------------
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
    /* FIX: isReloadTemp era assente qui — ra_simplify() lo legge sempre
     * nel ramo di spill ottimistico (vedi nota di modulo sopra). */
    g.isReloadTemp = arena_alloc(arena, (size_t)n * sizeof(char));
    memset(g.isReloadTemp, 0, (size_t)n * sizeof(char));

    for (int i = 0; i < n; i++) {
        int_vector_init(&g.adj[i],IG_ADJ_INITIAL_CAPACITY);
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
    g.isReloadTemp = arena_alloc(arena, (size_t)n * sizeof(char)); /* FIX: vedi build_clique */
    memset(g.isReloadTemp, 0, (size_t)n * sizeof(char));
    for (int i = 0; i < n; i++) {
        int_vector_init(&g.adj[i],IG_ADJ_INITIAL_CAPACITY);
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
        assert(g.color[0] == 5 && "il biased coloring deve riusare il colore 5 del partner");

        printf("PASS 4 ok: biased coloring riusa il colore 5 del partner non interferente.\n");
        free(pl.pairs);
        arena_destroy(arena);
    }

    /* ================================================================
     * PASS 5: preferenza reload-temp — cricca di k+1 nodi con il nodo 0
     * flaggato isReloadTemp=1 e spillCost artificialmente MINIMO (ratio
     * spillCost/degree piu' basso di tutti). Senza la de-prioritizzazione
     * documentata in interference.h, l'euristica ottimistica sceglierebbe
     * SEMPRE il nodo col ratio piu' basso in assoluto, cioe' il reload
     * temp — respillandolo all'infinito senza mai alleviare la vera
     * pressione sui registri (vedi bug storico "15+ stack slot" citato
     * nel modulo). ra_simplify() deve invece preferire un candidato REALE
     * (bestReal) anche se il suo ratio e' peggiore.
     * ================================================================ */
    {
        int n = PHYS_ALLOCATABLE + 1; /* 15: nessun nodo ha grado<k all'inizio */
        Arena *arena = arena_create(0);
        IGraph g = build_clique(n, arena);

        /* nodo 0: reload temp con costo di spill quasi nullo -> ratio minimo */
        g.isReloadTemp[0] = 1;
        g.spillCost[0]    = 1;
        /* tutti gli altri: nodi "reali" con costo alto -> ratio alto */
        for (int v = 1; v < n; v++) g.spillCost[v] = 10000;

        int *stack;
        int stackLen = ra_simplify(&g, n, &stack);
        assert(stackLen == n);

        /* stack[0] = primo nodo rimosso = la prima scelta ottimistica
         * (nessun nodo bucketizzato al primo giro: tutti grado k). */
        assert(stack[0] != 0 &&
               "il primo spill ottimistico non deve mai cadere sul reload-temp "
               "quando esiste un candidato reale, indipendentemente dal ratio");

        printf("PASS 5 ok: ra_simplify preferisce un candidato reale al reload-temp "
               "anche con ratio spillCost/degree peggiore.\n");
        free(stack);
        arena_destroy(arena);
    }

    /* ================================================================
     * PASS 6: hint multipli — il primo partner in ordine di scansione ha
     * un colore gia' assegnato ma NON disponibile (escluso via excl[]);
     * hint_color deve scartarlo e proseguire al partner successivo, la
     * cui colorazione E' disponibile.
     *
     * v0 (da colorare) non interferisce con A ne' con B. A e' precolorato
     * a 0, ma 0 e' forzato non disponibile per v0 tramite excl[0]=bit0.
     * B e' precolorato a 3 (disponibile). pl = [(v0,A), (v0,B)] in questo
     * ordine: hint_color deve saltare A e restituire 3 da B.
     * ================================================================ */
    {
        int n = 3; /* 0=v0, 1=A, 2=B */
        Arena *arena = arena_create(0);
        IGraph g;
        g.n = n;
        g.matrix = arena_alloc(arena, sizeof(uint64_t));
        memset(g.matrix, 0, sizeof(uint64_t)); /* nessuna interferenza tra i 3 nodi */
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

        g.color[0] = -1;   /* v0: da colorare */
        g.color[1] = 0;    /* A: gia' colorato a 0 */
        g.color[2] = 3;    /* B: gia' colorato a 3 */
        g.excl[0]  = (1u << 0); /* v0 non puo' MAI ricevere il colore 0 */

        int stack[1] = { 0 };
        int spilled[1];
        PartnerList pl = { NULL, 0, 0 };
        pl.pairs = malloc(2 * sizeof *pl.pairs);
        pl.pairs[0] = (PartnerPair){ .u = 0, .v = 1 }; /* A: colore 0, non disponibile */
        pl.pairs[1] = (PartnerPair){ .u = 0, .v = 2 }; /* B: colore 3, disponibile */
        pl.count = 2; pl.cap = 2;

        int nSpilled = ra_select_colors(&g, n, stack, 1, spilled, &pl);
        assert(nSpilled == 0);
        assert(g.color[0] == 3 &&
               "hint del primo partner (colore escluso) deve essere scartato; "
               "il secondo partner (colore 3, disponibile) va usato");

        printf("PASS 6 ok: hint_color scarta un partner con colore non disponibile "
               "e usa correttamente il successivo.\n");
        free(pl.pairs);
        arena_destroy(arena);
    }

    printf("\nTutti i test ra_color sono passati.\n");
    return 0;
}