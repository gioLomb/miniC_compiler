/**
 * @file loop.c
 * @brief Loop detection and pre-header construction — implementation.
 *
 * See loop.h for the module overview and public API documentation.
 *
 * Internal organisation
 * ---------------------
 *  loop_compute_dominators — iterative dominator dataflow (intersection).
 *  loop_dominates          — O(1) bit-set dominance query.
 *  collectBody             — backward BFS to gather loop body blocks.
 *  loop_find               — scan back-edges, call collectBody, record exits.
 *  loop_build_pre_header   — append synthetic block, re-route predecessors.
 *
 * Predecessor lookups
 * --------------------
 * Both loop_compute_dominators (intersection of predecessor Dom sets at
 * every fixed-point iteration) and collectBody (reverse BFS from the
 * back-edge tail, called once per natural loop found) need, for a given
 * block, the set of blocks whose succ[] targets it. Both now use a shared
 * PredList (ir.h, built via ir_build_pred_list()) instead of independently
 * re-scanning every block's succ[] to find this — the old approach cost
 * O(nBlocks) per lookup, repeated O(nBlocks) times per fixed-point
 * iteration in loop_compute_dominators (O(nBlocks^2) per pass), and was
 * additionally capped at a fixed fan-in of 2 predecessors per block in
 * collectBody's old inline preds[n][2] array — a latent buffer overflow
 * for any block with more than two incoming edges (e.g. several branches
 * converging on the same join block). PredList has no such cap.
 */

#include <stdlib.h>
#include <string.h>
#include "loop.h"

/* =========================================================================
 * Dominator computation
 * =========================================================================
 *
 * Standard iterative algorithm (Cooper et al.):
 *   Dom[0]   = {0}          (entry dominated only by itself)
 *   Dom[b>0] = U − 1        (universal set: dominated by every block)
 *
 * Each iteration:
 *   Dom[b] = {b} ∪ ∩{ Dom[p] | p ∈ preds(b) }
 *
 * Converges because Dom sets can only shrink (intersection is monotone)
 * and the lattice is finite.
 * ========================================================================= */

/**
 * @brief Compute Dom[] for all blocks via iterative fixed-point dataflow.
 *
 * Allocates one BitSet per block from @p arena; each BitSet has @p words
 * uint64_t words (== ceil(blockCount / 64)).
 *
 * The entry block starts with Dom[0] = {0}; all other blocks start with the
 * universal set (all bits 1), which ensures the intersection-based update
 * produces a correct upper bound on the first pass even if predecessors
 * have not yet been processed in the current iteration.
 *
 * A leftover-bits mask is applied to the last word of the universal sets so
 * that block indices past blockCount are never spuriously set.
 *
 * Predecessor sets are looked up via a PredList built once at the start of
 * this function (see module header) and reused across every iteration of
 * the fixed-point loop below.
 */
BitSet *loop_compute_dominators(IRFunction *f, int words, Arena *arena) {
    int n = f->blockCount;
    BitSet *Dom = arena_alloc(arena, (size_t)n * sizeof(BitSet));

    // predecessor list built once, reused across every fixed-point iteration
    // (succ[] is read-only throughout this function: safe to build up front)
    PredList preds = ir_build_pred_list(f, arena);

    // initialise Dom sets
    for (int b = 0; b < n; b++) {
        Dom[b] = bitset_new(arena, words);
        if (b == 0) {
            // entry block: dominated only by itself
            bitset_set(&Dom[b], 0);
        } else {
            // all other blocks: start with universal set (all bits 1)
            memset(Dom[b].bits, 0xFF, (size_t)words * sizeof(uint64_t));
            // clear bits beyond the last valid block index in the last word
            int leftover = n % 64;
            if (leftover) Dom[b].bits[words - 1] = (1ULL << leftover) - 1;
        }
    }

    // scratch sets for the intersection step
    BitSet inter = bitset_new(arena, words);
    BitSet tmp   = bitset_new(arena, words);

    int changed = 1;
    while (changed) {
        changed = 0;
        // skip block 0: its Dom set is fixed to {0}
        for (int b = 1; b < n; b++) {
            int start = preds.predStart[b];
            int cnt   = preds.predCount[b];
            if (cnt == 0) continue; // block unreachable: no predecessors found

            // intersect Dom sets of all predecessors, resolved via the
            // precomputed CSR list instead of re-scanning every block's
            // succ[] to find them
            bitset_copy(&inter, &Dom[preds.predData[start]]); // first pred: copy
            for (int i = 1; i < cnt; i++) {
                int p = preds.predData[start + i];
                // subsequent preds: AND word by word (intersection)
                for (int w = 0; w < words; w++)
                    inter.bits[w] &= Dom[p].bits[w];
            }

            // Dom[b] = {b} ∪ inter
            bitset_copy(&tmp, &inter);
            bitset_set(&tmp, b);

            if (!bitset_equal(&Dom[b], &tmp)) {
                bitset_copy(&Dom[b], &tmp);
                changed = 1; // Dom set shrank: keep iterating
            }
        }
    }
    return Dom;
}

int loop_dominates(BitSet *Dom, int a, int b) {
    return bitset_test(&Dom[b], a);
}

/* =========================================================================
 * Loop body collection
 * =========================================================================
 *
 * Given a back-edge tail→header, the loop body is the set of all blocks
 * that can reach `tail` going backwards through the CFG without passing
 * through `header` (which is the only entry point of the loop).
 *
 * Algorithm: reverse-BFS starting from `tail`, treating `header` as already
 * visited so the BFS never crosses it.  Both `header` and `tail` are always
 * included in the body (header is the entry; tail is the back-edge source).
 * ========================================================================= */

/**
 * @brief Collect the loop body for the back-edge tail→header via reverse BFS.
 *
 * Walks predecessor edges via the precomputed @p preds CSR list (built once
 * by the caller, loop_find(), and shared across every natural loop found —
 * see module header) rather than reconstructing a reverse-adjacency
 * structure from scratch on every call. All discovered block indices are
 * written into @p body in BFS order; @p *bodyCount is set to the total count.
 *
 * @param f          IR function containing the CFG.
 * @param header     Loop-header block index (BFS root; always included).
 * @param tail       Back-edge source block index (BFS seed).
 * @param body       Output array; caller must provide space for n entries.
 * @param bodyCount  Set to the number of entries written into @p body.
 * @param preds      Precomputed predecessor list for @p f (ir_build_pred_list()).
 * @param arena      Arena for internal BFS auxiliary arrays.
 */
static void collectBody(IRFunction *f, int header, int tail,
                        int *body, int *bodyCount, const PredList *preds,
                        Arena *arena) {
    int n = f->blockCount;

    // membership flag for the BFS, arena-scratch, reset per call
    char *inBody = arena_alloc(arena, (size_t)n);
    memset(inBody, 0, (size_t)n);

    // BFS queue (arena-allocated; at most n entries)
    int *queue = arena_alloc(arena, (size_t)n * sizeof(int));
    int head = 0, tail_q = 0;

    // seed: both header and tail are always in the body
    inBody[header] = inBody[tail] = 1;
    queue[tail_q++] = tail;

    while (head < tail_q) {
        int b = queue[head++];
        // walk b's actual predecessors via the CSR list — unlike the old
        // fixed-size preds[n][2] array this had no cap on fan-in, so a
        // join block with more than two incoming edges is handled correctly
        int start = preds->predStart[b];
        int cnt   = preds->predCount[b];
        for (int k = 0; k < cnt; k++) {
            int p = preds->predData[start + k];
            if (!inBody[p]) {
                inBody[p] = 1;
                queue[tail_q++] = p;
            }
        }
    }

    // collect all body blocks in index order for deterministic output
    *bodyCount = 0;
    for (int b = 0; b < n; b++)
        if (inBody[b]) body[(*bodyCount)++] = b;
}

/* =========================================================================
 * Loop detection
 * ========================================================================= */

/**
 * @brief Find natural loops via back-edge scanning.
 *
 * For every CFG edge b→h, if h dominates b it is a back-edge defining a
 * natural loop.  For each such edge:
 *   1. Call collectBody() to gather all blocks in the loop body.
 *   2. Scan body blocks for exit blocks (have a successor outside the body).
 *      Each distinct exit block is recorded at most once in L->exits[].
 *
 * A single PredList is built once at the top of this function and shared
 * across every collectBody() call below — succ[] is never mutated while
 * loop_find() runs, so the list stays valid for all loops discovered in
 * this call (see module header for the rationale).
 *
 * Stops early if MAX_LOOPS loops have already been found.
 */
int loop_find(IRFunction *f, BitSet *Dom, Loop *loops, Arena *arena) {
    int n = f->blockCount, nLoops = 0;
    // reuse a single body scratch buffer across all loops
    int *body = arena_alloc(arena, (size_t)n * sizeof(int));

    // predecessor list built once, reused by every collectBody() call below
    // instead of each call reconstructing its own reverse-adjacency lists
    PredList preds = ir_build_pred_list(f, arena);

    for (int b = 0; b < n && nLoops < MAX_LOOPS; b++) {
        for (int k = 0; k < 2; k++) {
            int h = f->blocks[b].bb.succ[k];
            // back-edge: successor h dominates b
            if (h < 0 || !loop_dominates(Dom, h, b)) continue;

            Loop *L = &loops[nLoops++];
            L->header    = h;
            L->preHeader = -1; // not yet created
            L->exitCount = 0;

            // collect body; copy into arena-allocated array owned by Loop
            int bodyCount = 0;
            collectBody(f, h, b, body, &bodyCount, &preds, arena);
            L->body      = arena_alloc(arena, (size_t)bodyCount * sizeof(int));
            L->bodyCount = bodyCount;
            memcpy(L->body, body, (size_t)bodyCount * sizeof(int));

            // mark body membership for O(1) exit-block test below
            char *inBody = arena_alloc(arena, (size_t)n);
            memset(inBody, 0, (size_t)n);
            for (int i = 0; i < bodyCount; i++) inBody[body[i]] = 1;

            // find exit blocks: body blocks with at least one out-of-loop successor
            for (int i = 0; i < bodyCount && L->exitCount < 64; i++) {
                int bl = body[i];
                for (int s = 0; s < 2; s++) {
                    int succ = f->blocks[bl].bb.succ[s];
                    if (succ < 0 || inBody[succ]) continue; // not an exit edge

                    // record bl as an exit block (at most once)
                    int already = 0;
                    for (int e = 0; e < L->exitCount; e++)
                        if (L->exits[e] == bl) { already = 1; break; }
                    if (!already) L->exits[L->exitCount++] = bl;
                    break; // one out-of-loop successor is enough to classify bl as exit
                }
            }
        }
    }
    return nLoops;
}

/* =========================================================================
 * Pre-header insertion
 * ========================================================================= */

/**
 * @brief Insert a synthetic pre-header block before @p L->header.
 *
 * The new block is appended to f->blocks[] (so its index = old blockCount)
 * and given an empty instruction range [f->count, f->count) — instructions
 * will be inserted into it later by LICM or SR.
 *
 * Every predecessor of the header that is NOT part of the loop body is
 * re-routed to the pre-header: their succ[] entry pointing to header is
 * changed to point to the new block.  predCount on both the header and the
 * pre-header is updated accordingly.
 *
 * This function mutates succ[] directly (predecessor rerouting), so it
 * necessarily works on the live succ[] arrays rather than any cached
 * PredList — any PredList built before this call becomes stale afterwards
 * and must be rebuilt by the next pass that needs one (both callers,
 * licm_optimize() and sr_optimize(), only build/use a PredList inside
 * loop_compute_dominators()/loop_find(), which run once per pass before
 * any pre-header is created for that pass — no staleness issue in
 * practice, but any future caller that interleaves pre-header creation
 * with PredList-based lookups must rebuild the list afterwards).
 *
 * A temporary inBody[] membership array is allocated from the heap (not the
 * arena) because the Loop's arena may have been destroyed by the time this
 * function is called; it is freed before returning.
 */
int loop_build_pre_header(IRFunction *f, Loop *L) {
    int header = L->header;

    // grow block array if needed
    if (f->blockCount == f->blockCap) {
        f->blockCap = f->blockCap ? f->blockCap * 2 : 16;
        f->blocks = realloc(f->blocks, (size_t)f->blockCap * sizeof(IRBlock));
    }

    int phIdx   = f->blockCount++;
    IRBlock *ph = &f->blocks[phIdx];

    // empty block: instructions will be inserted here by LICM/SR
    ph->bb.range.start   = ph->bb.range.end = f->count;
    ph->bb.succ[0] = header; // pre-header falls through to the loop header
    ph->bb.succ[1] = -1;
    ph->predCount  = 0;

    // build body membership for O(1) lookup during predecessor re-routing
    char *inBody = calloc((size_t)(phIdx + 1), 1);
    for (int i = 0; i < L->bodyCount; i++) inBody[L->body[i]] = 1;

    // re-route non-body predecessors of header through the pre-header
    for (int b = 0; b < phIdx; b++)
        for (int k = 0; k < 2; k++)
            if (f->blocks[b].bb.succ[k] == header && !inBody[b]) {
                f->blocks[b].bb.succ[k] = phIdx;  // b now jumps to pre-header
                f->blocks[header].predCount--;     // header loses one predecessor
                ph->predCount++;                   // pre-header gains one
            }

    free(inBody);

    // header gains the pre-header as its new (only) non-back-edge predecessor
    f->blocks[header].predCount++;
    L->preHeader = phIdx;
    return phIdx;
}