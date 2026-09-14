#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "interference.h"
#include "regalloc_utils.h"
#include "instr_query.h"
#include "reg_class.h"

/**
 * @brief Map the unordered pair (i, j) to its flat lower-triangular index.
 *
 * After the swap, i > j is guaranteed (diagonal excluded since self-loops are
 * meaningless).  Row i starts at i*(i-1)/2 and column j is j steps into it.
 */
static inline long ig_tri_idx(int i, int j) {
    if(i<0 || j<0 || i==j) return 0;
    // enforce canonical ordering: larger id becomes the row index
    if (i < j) { int t = i; i = j; j = t; }
    // standard lower-triangular formula: Σ_{k=0}^{i-1} k  = i*(i-1)/2
    return (long)i * (i - 1) / 2 + j;
}


int ig_has_edge(const IGraph *g, int i, int j) {
    if (i < 0 || j < 0 || i == j) return 0;
    long idx = ig_tri_idx(i, j);
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
    long idx = ig_tri_idx(i, j);
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
static void ig_precolor_physicals(IGraph *g, int classVregCount, int allocatable) {
    for (int p = 0; p < allocatable; p++)
        g->color[classVregCount + p] = p;
}
static void ig_accumulate_spill_cost(IGraph *g, const MachInstr *in,
                                      int classVregCount, RegClass cls, int *tmpArr) {
    int w = regalloc_spill_weight(in->loopDepth);
    int n;

    instr_uses(in, classVregCount, cls, tmpArr, &n);
    for (int k = 0; k < n; k++)
        // only vregs (< classVregCount) get a spill cost: physicals are
        // never spilled, so their entries would be wasted/out-of-range
        if (tmpArr[k] < classVregCount) g->spillCost[tmpArr[k]] += w;

    instr_defs(in, classVregCount, cls, tmpArr, &n);
    for (int k = 0; k < n; k++)
        if (tmpArr[k] < classVregCount) g->spillCost[tmpArr[k]] += w;
}

static void ig_add_definition_edges(IGraph *g, const MachInstr *in,
                                     int classVregCount, RegClass cls,
                                     const LiveSet *liveAfterInstr) {
    int defs[MAX_EXPLICIT_DEFS], nd, idefs[LIVENESS_MAX_IDS], nid;
    instr_defs(in, classVregCount, cls, defs, &nd);
    instr_implicit_defs(in, classVregCount, cls, idefs, &nid);

    // explicit defs: each one interferes with everything still live right
    // after this instruction (both must occupy distinct registers)
    int id;
    for (int d = 0; d < nd; d++)
        for (LiveSetIter it = LIVESET_ITER(liveAfterInstr); LIVESET_NEXT(&it, &id); )
            ig_add_edge(g, defs[d], id);

    // implicit defs (e.g. CALL clobbers): same interference rule, separate
    // loop since defs[]/idefs[] have different sizes and no shared index
    for (int d = 0; d < nid; d++)
        for (LiveSetIter it = LIVESET_ITER(liveAfterInstr); LIVESET_NEXT(&it, &id); )
            ig_add_edge(g, idefs[d], id);
}

static void ig_apply_constraint_masks(IGraph *g, const MachInstr *in,
                                       int classVregCount, RegClass cls,
                                       int callerSavedCount,
                                       const LiveSet *liveAfterInstr) {
    int id;

    if (in->op == MACH_CALL) {
        // forbid every caller-saved color (both classes) for anything live
        // across the call, and flag it so the colorer prefers callee-saved
        uint32_t mask = (callerSavedCount >= 32) ? ~0u : ((1U << callerSavedCount) - 1);
        for (LiveSetIter it = LIVESET_ITER(liveAfterInstr); LIVESET_NEXT(&it, &id); ) {
            if (id < classVregCount) {
                g->excl[id] |= mask;
                g->crossesCall[id] = 1;
            }
        }
    } else if (cls == RC_INT && (in->op == MACH_IDIV || in->op == MACH_CQO)) {
        // IDIV/CQO implicitly clobber RAX:RDX: only relevant for the int class
        uint32_t mask = (1U << PHYS_RAX) | (1U << PHYS_RDX);
        for (LiveSetIter it = LIVESET_ITER(liveAfterInstr); LIVESET_NEXT(&it, &id); )
            if (id < classVregCount) g->excl[id] |= mask;
    } else if (cls == RC_INT && instr_is_setcc(in->op)) {
        // SETcc writes %al (alias of RAX): forbid RAX for live vregs
        uint32_t mask = (1U << PHYS_RAX);
        for (LiveSetIter it = LIVESET_ITER(liveAfterInstr); LIVESET_NEXT(&it, &id); )
            if (id < classVregCount) g->excl[id] |= mask;
    }
}


IGraph ig_build(const MachFunction *f, const BasicBlock *blocks, int nBlocks,
                RegClass cls, int classVregCount, const LiveSet *liveAfter,
                int firstSpillVreg, Arena *arena) {
    const RegClassInfo *ci = reg_class_info(cls);
    int totalNodes = classVregCount + ci->allocatable;

    IGraph g;
    ig_alloc_storage(&g, totalNodes, arena);
    ig_precolor_physicals(&g, classVregCount, ci->allocatable);
    // vregs introduced by a previous spill round get deprioritized in Simplify
    ig_mark_reload_temps(&g, firstSpillVreg, classVregCount);

    int tmpArr[LIVENESS_MAX_IDS];

    for (int b = 0; b < nBlocks; b++) {
        for (int i = blocks[b].range.start; i < blocks[b].range.end; i++) {
            const MachInstr *in = &f->instrs[i];

            ig_accumulate_spill_cost(&g, in, classVregCount, cls, tmpArr);
            ig_add_definition_edges(&g, in, classVregCount, cls, &liveAfter[i]);
            ig_apply_constraint_masks(&g, in, classVregCount, cls,
                                      ci->callerSavedCount, &liveAfter[i]);
        }
    }

    return g;
}