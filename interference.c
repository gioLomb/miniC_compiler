#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "interference.h"
#include "regalloc_utils.h"


/**
 * @brief Map the unordered pair (i, j) to its flat lower-triangular index.
 *
 * After the swap, i > j is guaranteed (diagonal excluded since self-loops are
 * meaningless).  Row i starts at i*(i-1)/2 and column j is j steps into it.
 */
static inline long tri_idx(int i, int j) {
    // enforce canonical ordering: larger id becomes the row index
    if (i < j) { int t = i; i = j; j = t; }
    // standard lower-triangular formula: Σ_{k=0}^{i-1} k  = i*(i-1)/2
    return (long)i * (i - 1) / 2 + j;
}

/**
 * @brief Test whether edge (i, j) exists in the interference graph.
 *
 * Converts the pair to a flat index, selects the correct uint64_t word
 * (idx >> 6 = idx / 64) and extracts the appropriate bit (idx & 63).
 */
static inline int ig_has_edge(const IGraph *g, int i, int j) {
    long idx = tri_idx(i, j);
    // word index: which uint64_t holds this bit
    // bit offset: position within that word (0..63)
    return (int)((g->matrix[idx >> 6] >> (idx & 63)) & 1ULL);
}

/**
 * @brief Insert edge (i, j) into the interference graph.
 *
 * Maintains both representations atomically:
 *   - The triangular bit matrix provides O(1) duplicate detection.
 *   - The per-node adjacency lists support neighbour iteration during Simplify.
 * Both i's and j's degree counters are incremented (the graph is undirected).
 */
static void ig_add_edge(IGraph *g, int i, int j) {
    // reject degenerate inputs: self-loops and invalid ids
    if (i == j || i < 0 || j < 0) return;
    // O(1) duplicate check via bit matrix before touching adjacency lists
    if (ig_has_edge(g, i, j)) return;

    // set the bit in the triangular matrix
    long idx = tri_idx(i, j);
    g->matrix[idx >> 6] |= 1ULL << (idx & 63);

    // update both adjacency lists (undirected edge: each node lists the other)
    AdjList *ai = &g->adj[i];
    int_vector_push(ai, j);

    AdjList *aj = &g->adj[j];
    int_vector_push(aj, i);

    // bump degrees (used by Simplify to pick low-degree nodes for colouring)
    g->degree[i]++;
    g->degree[j]++;
}

void ig_free(IGraph *g) {
    if (!g || !g->adj) return;
    // only the IntVector backing arrays are heap-allocated; fixed-size arrays
    // are in the caller's arena and must not be freed individually
    for (int i = 0; i < g->n; i++) int_vector_free(&g->adj[i]);
}


/* =========================================================================
 * ig_build — helpers (allocation, precoloring, per-instruction processing)
 * ========================================================================= */

/**
 * @brief Allocate and default-initialise every parallel array of @p g.
 *
 * Sets g->n and allocates matrix/adj/degree/color/active/excl/spillCost/
 * crossesCall/isReloadTemp from @p arena, applying the same defaults
 * ig_build() used inline before this refactor (color = -1, active = true,
 * everything else zeroed).
 */
static void ig_alloc_storage(IGraph *g, int totalNodes, Arena *arena) {
    g->n = totalNodes;

    // triangular bit matrix: n*(n-1)/2 bits, +1 word safety margin
    long   nbits       = (long)totalNodes * (totalNodes - 1) / 2;
    size_t matrixWords = (size_t)((nbits + 63) / 64 + 1);
    g->matrix = arena_alloc(arena, matrixWords * sizeof(uint64_t));
    memset(g->matrix, 0, matrixWords * sizeof(uint64_t));

    // adjacency list headers (arena); backing arrays heap-allocated by IntVector
    g->adj = arena_alloc(arena, (size_t)totalNodes * sizeof(AdjList));
    for (int i = 0; i < totalNodes; i++)
        int_vector_init(&g->adj[i], IG_ADJ_INITIAL_CAPACITY);

    // parallel metadata arrays
    g->degree       = arena_alloc(arena, (size_t)totalNodes * sizeof(int));
    g->color        = arena_alloc(arena, (size_t)totalNodes * sizeof(int));
    g->active       = arena_alloc(arena, (size_t)totalNodes * sizeof(bool));
    g->excl         = arena_alloc(arena, (size_t)totalNodes * sizeof(uint32_t));
    g->spillCost    = arena_alloc(arena, (size_t)totalNodes * sizeof(int));
    g->crossesCall  = arena_alloc(arena, (size_t)totalNodes * sizeof(char));
    g->isReloadTemp = arena_alloc(arena, (size_t)totalNodes * sizeof(char));

    memset(g->degree,       0, (size_t)totalNodes * sizeof(int));
    memset(g->excl,         0, (size_t)totalNodes * sizeof(uint32_t));
    memset(g->spillCost,    0, (size_t)totalNodes * sizeof(int));
    memset(g->crossesCall,  0, (size_t)totalNodes * sizeof(char));
    memset(g->isReloadTemp, 0, (size_t)totalNodes * sizeof(char));

    // color = -1 (uncoloured): 0xFF fills every byte, which is -1 in two's complement
    memset(g->color,  0xFF, (size_t)totalNodes * sizeof(int));
    // active = true: sizeof(bool)==1, so memset with 1 is correct
    memset(g->active, 1,    (size_t)totalNodes * sizeof(bool));
}

/**
 * @brief Flag every vreg in [firstSpillVreg, nextVreg) as a reload/spill temp.
 *
 * Clamps firstSpillVreg defensively so a caller passing a negative or
 * stale value never corrupts the loop bounds (see interference.h doc).
 */
static void ig_mark_reload_temps(IGraph *g, int firstSpillVreg, int nextVreg) {
    if (firstSpillVreg >= nextVreg) return;
    int from = firstSpillVreg < 0 ? 0 : firstSpillVreg;
    for (int v = from; v < nextVreg; v++) g->isReloadTemp[v] = 1;
}

/**
 * @brief Pre-colour every physical-register node: color[nextVreg+p] = p.
 */
static void ig_precolor_physicals(IGraph *g, int nextVreg) {
    for (int p = 0; p < PHYS_ALLOCATABLE; p++)
        g->color[nextVreg + p] = p;
}

/**
 * @brief Add @p in's loop-depth-weighted spill cost to every vreg it uses or defines.
 *
 * @param tmpArr Scratch array (capacity LIVENESS_MAX_IDS), reused by the caller
 *               across instructions to avoid a stack allocation per call.
 */
static void ig_accumulate_spill_cost(IGraph *g, const MachInstr *in,
                                      int nextVreg, int *tmpArr) {
    int w = regalloc_spill_weight(in->loopDepth);
    int n;

    instr_uses(in, nextVreg, tmpArr, &n);
    for (int k = 0; k < n; k++)
        if (tmpArr[k] < nextVreg) g->spillCost[tmpArr[k]] += w;

    instr_defs(in, nextVreg, tmpArr, &n);
    for (int k = 0; k < n; k++)
        if (tmpArr[k] < nextVreg) g->spillCost[tmpArr[k]] += w;
}

/**
 * @brief Add interference edges between every register @p in defines
 *        (explicit + implicit) and every register live after @p in.
 *
 * Liveness rule: a register defined at instruction i interferes with
 * every register live immediately after i, since both must occupy
 * distinct physical locations at the moment i commits its result.
 */
static void ig_add_definition_edges(IGraph *g, const MachInstr *in,
                                     int nextVreg, const LiveSet *liveAfterInstr) {
    int defs[MAX_EXPLICIT_DEFS], nd, idefs[LIVENESS_MAX_IDS], nid;
    instr_defs(in, nextVreg, defs, &nd);
    instr_implicit_defs(in, nextVreg, idefs, &nid);   // e.g. CALL clobbers RAX

    int id;
    for (int d = 0; d < nd; d++)
        for (LiveSetIter it = LIVESET_ITER(liveAfterInstr); LIVESET_NEXT(&it, &id); )
            ig_add_edge(g, defs[d], id);

    for (int d = 0; d < nid; d++)
        for (LiveSetIter it = LIVESET_ITER(liveAfterInstr); LIVESET_NEXT(&it, &id); )
            ig_add_edge(g, idefs[d], id);
}

/**
 * @brief Apply excl[]/crossesCall[] constraints implied by @p in's opcode.
 *
 * Covers the three sources of forbidden colours beyond plain interference
 * edges: CALL (all caller-saved forbidden + crossesCall flag), IDIV/CQO
 * (RAX/RDX forbidden), SETcc (RAX forbidden — writes %al).
 */
static void ig_apply_constraint_masks(IGraph *g, const MachInstr *in,
                                       int nextVreg, const LiveSet *liveAfterInstr) {
    int id;

    if (in->op == MACH_CALL) {
        for (LiveSetIter it = LIVESET_ITER(liveAfterInstr); LIVESET_NEXT(&it, &id); ) {
            if (id < nextVreg) {
                g->excl[id] |= ((1U << PHYS_CALLER_SAVED_COUNT) - 1);
                g->crossesCall[id] = 1;
            }
        }
    } else if (in->op == MACH_IDIV || in->op == MACH_CQO) {
        uint32_t mask = (1U << PHYS_RAX) | (1U << PHYS_RDX);
        for (LiveSetIter it = LIVESET_ITER(liveAfterInstr); LIVESET_NEXT(&it, &id); )
            if (id < nextVreg) g->excl[id] |= mask;
    } else if (regalloc_is_setcc(in->op)) {
        uint32_t mask = (1U << PHYS_RAX);
        for (LiveSetIter it = LIVESET_ITER(liveAfterInstr); LIVESET_NEXT(&it, &id); )
            if (id < nextVreg) g->excl[id] |= mask;
    }
}

/* =========================================================================
 * ig_build — main graph construction (orchestration only)
 * ========================================================================= */

IGraph ig_build(const MachFunction *f, const BasicBlock *blocks, int nBlocks,
                int nextVreg, const LiveSet *liveAfter, int firstSpillVreg,
                Arena *arena) {

    int totalNodes = nextVreg + PHYS_ALLOCATABLE;

    IGraph g;
    ig_alloc_storage(&g, totalNodes, arena);
    ig_precolor_physicals(&g, nextVreg);
    ig_mark_reload_temps(&g, firstSpillVreg, nextVreg);

    // scratch array reused across instructions to hold extracted def/use ids
    int tmpArr[LIVENESS_MAX_IDS];

    for (int b = 0; b < nBlocks; b++) {
        for (int i = blocks[b].range.start; i < blocks[b].range.end; i++) {
            const MachInstr *in = &f->instrs[i];

            ig_accumulate_spill_cost(&g, in, nextVreg, tmpArr);
            ig_add_definition_edges(&g, in, nextVreg, &liveAfter[i]);
            ig_apply_constraint_masks(&g, in, nextVreg, &liveAfter[i]);
        }
    }

    return g;
}