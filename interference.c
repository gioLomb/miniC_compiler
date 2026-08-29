#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "interference.h"
#include "regalloc_utils.h"

/* =========================================================================
 * Triangular matrix helpers
 * =========================================================================
 * The interference matrix stores one bit per unordered pair {i, j} with i≠j.
 * Pairs are mapped to a flat index using the lower-triangular formula so only
 * n*(n-1)/2 bits are needed instead of the full n*n square matrix.
 * ========================================================================= */

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
    for (int i = 0; i < g->n; i++)
        int_vector_free(&g->adj[i]);
}

/* =========================================================================
 * ig_build — main graph construction
 * ========================================================================= */

IGraph ig_build(const MachFunction *f, const BasicBlock *blocks, int nBlocks,
                int nextVreg, const LiveSet *liveAfter, int firstSpillVreg,
                Arena *arena) {

    // total node count: virtual registers + one node per allocatable physical reg
    int totalNodes = nextVreg + PHYS_ALLOCATABLE;

    IGraph g;
    g.n = totalNodes;

    /* --- Triangular bit matrix ------------------------------------------ */
    // need n*(n-1)/2 bits for all unordered pairs with i>j; +1 word safety margin
    long   nbits       = (long)totalNodes * (totalNodes - 1) / 2;
    size_t matrixWords = (size_t)((nbits + 63) / 64 + 1);
    g.matrix = arena_alloc(arena, matrixWords * sizeof(uint64_t));
    memset(g.matrix, 0, matrixWords * sizeof(uint64_t));

    /* --- Adjacency lists ------------------------------------------------- */
    // headers are arena-allocated; backing data arrays are heap-allocated by IntVector
    g.adj = arena_alloc(arena, (size_t)totalNodes * sizeof(AdjList));
    for (int i = 0; i < totalNodes; i++)
        int_vector_init(&g.adj[i], IG_ADJ_INITIAL_CAPACITY);

    /* --- Parallel metadata arrays --------------------------------------- */
    g.degree        = arena_alloc(arena, (size_t)totalNodes * sizeof(int));
    g.color         = arena_alloc(arena, (size_t)totalNodes * sizeof(int));
    g.active        = arena_alloc(arena, (size_t)totalNodes * sizeof(bool));
    g.excl          = arena_alloc(arena, (size_t)totalNodes * sizeof(uint32_t));
    g.spillCost     = arena_alloc(arena, (size_t)totalNodes * sizeof(int));
    g.crossesCall   = arena_alloc(arena, (size_t)totalNodes * sizeof(char));
    g.isReloadTemp  = arena_alloc(arena, (size_t)totalNodes * sizeof(char));

    memset(g.degree,       0,    (size_t)totalNodes * sizeof(int));
    memset(g.excl,         0,    (size_t)totalNodes * sizeof(uint32_t));
    memset(g.spillCost,    0,    (size_t)totalNodes * sizeof(int));
    memset(g.crossesCall,  0,    (size_t)totalNodes * sizeof(char));
    memset(g.isReloadTemp, 0,    (size_t)totalNodes * sizeof(char));

    // color = -1 (uncoloured): 0xFF fills every byte, which is -1 in two's complement
    memset(g.color,  0xFF, (size_t)totalNodes * sizeof(int));
    // active = true: sizeof(bool)==1, so memset with 1 is correct
    memset(g.active, 1,    (size_t)totalNodes * sizeof(bool));

    /* --- Reload-temp flag -------------------------------------------------
     * Every vreg id in [firstSpillVreg, nextVreg) was introduced by a
     * previous ra_spill_insert() round (see interference.h module doc).
     * Clamp the lower bound so a caller passing firstSpillVreg < 0 (or a
     * stale value larger than nextVreg) never corrupts the loop bounds. */
    if (firstSpillVreg < nextVreg) {
        int from = firstSpillVreg < 0 ? 0 : firstSpillVreg;
        for (int v = from; v < nextVreg; v++) g.isReloadTemp[v] = 1;
    }

    /* --- Pre-colour physical registers ---------------------------------- */
    // physical regs occupy node ids [nextVreg, nextVreg + PHYS_ALLOCATABLE)
    // their colour equals their physical index (0..PHYS_ALLOCATABLE-1)
    for (int p = 0; p < PHYS_ALLOCATABLE; p++)
        g.color[nextVreg + p] = p;

    /* --- Edge construction + metadata accumulation ---------------------- */
    // scratch array reused across instructions to hold extracted def/use ids
    int tmpArr[LIVENESS_MAX_IDS];

    for (int b = 0; b < nBlocks; b++) {
        for (int i = blocks[b].range.start; i < blocks[b].range.end; i++) {
            const MachInstr *in = &f->instrs[i];

            // spill cost weight: 10^loopDepth so hot-loop variables resist spilling
            int w = regalloc_spill_weight(in->loopDepth);

            // accumulate spill cost for every explicit use at this instruction
            int n;
            instr_uses(in, nextVreg, tmpArr, &n);
            for (int k = 0; k < n; k++)
                if (tmpArr[k] < nextVreg) g.spillCost[tmpArr[k]] += w;

            // accumulate spill cost for every explicit def at this instruction
            instr_defs(in, nextVreg, tmpArr, &n);
            for (int k = 0; k < n; k++)
                if (tmpArr[k] < nextVreg) g.spillCost[tmpArr[k]] += w;

            /* --- Interference edges from explicit and implicit defs --- */
            // liveness rule: every register defined at instruction i interferes
            // with every register live immediately after i, because both must
            // occupy distinct physical locations at the moment i commits its result
            int defs[MAX_EXPLICIT_DEFS], nd, idefs[LIVENESS_MAX_IDS], nid;
            instr_defs(in, nextVreg, defs, &nd);
            instr_implicit_defs(in, nextVreg, idefs, &nid);   // e.g. CALL clobbers RAX

            int id;

            // edges for explicit defs (e.g. destination of MOV, ADD, …)
            for (int d = 0; d < nd; d++) {
                for (LiveSetIter it = LIVESET_ITER(&liveAfter[i]);
                     LIVESET_NEXT(&it, &id); )
                    ig_add_edge(&g, defs[d], id);
            }

            // edges for implicit defs (ABI side effects not in explicit operands)
            for (int d = 0; d < nid; d++) {
                for (LiveSetIter it = LIVESET_ITER(&liveAfter[i]);
                     LIVESET_NEXT(&it, &id); )
                    ig_add_edge(&g, idefs[d], id);
            }

            /* --- Constraint masks for special instructions ------------- */

            if (in->op == MACH_CALL) {
                // CALL clobbers all caller-saved registers; any vreg live
                // across the call must not be assigned to one of them
                for (LiveSetIter it = LIVESET_ITER(&liveAfter[i]);
                     LIVESET_NEXT(&it, &id); ) {
                    if (id < nextVreg) {
                        // forbid all caller-saved colours (indices 0..PHYS_CALLER_SAVED_COUNT-1)
                        g.excl[id] |= ((1U << PHYS_CALLER_SAVED_COUNT) - 1);
                        // flag for colour selector: prefer callee-saved to avoid push/pop
                        g.crossesCall[id] = 1;
                    }
                }

            } else if (in->op == MACH_IDIV || in->op == MACH_CQO) {
                // IDIV reads and writes RAX (quotient) and RDX (remainder);
                // any vreg live across this must avoid both physical registers
                uint32_t mask = (1U << PHYS_RAX) | (1U << PHYS_RDX);
                for (LiveSetIter it = LIVESET_ITER(&liveAfter[i]);
                     LIVESET_NEXT(&it, &id); )
                    if (id < nextVreg) g.excl[id] |= mask;

            } else if (regalloc_is_setcc(in->op)) {
                // SETcc writes %al (low byte of RAX); a vreg in RAX would alias
                // the 1-byte result and corrupt it when widened — exclude RAX
                uint32_t mask = (1U << PHYS_RAX);
                for (LiveSetIter it = LIVESET_ITER(&liveAfter[i]);
                     LIVESET_NEXT(&it, &id); )
                    if (id < nextVreg) g.excl[id] |= mask;
            }
        }
    }

    return g;
}