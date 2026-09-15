/**
 * @file loop.c
 * @brief Loop detection and pre-header construction.

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
 * @brief Initialise Dom sets for all blocks prior to fixed-point iteration.
 *
 * Dom[0] = {0} (entry block).
 * Dom[b>0] = Universal set (all bits 1, masked to valid block count).
 */
static void loop_init_dominator_sets(BitSet *Dom, int n, int words, Arena *arena) {
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
            if (leftover) {
                Dom[b].bits[words - 1] = (1ULL << leftover) - 1;
            }
        }
    }
}

/**
 * @brief Intersect Dom sets of all predecessors for block @p b via CSR PredList.
 */
static void loop_intersect_predecessor_dominators(BitSet *inter, const BitSet *Dom,
                                             const PredList *preds, int b, int words) {
    int start = preds->predStart[b];

    // intersect Dom sets of all predecessors, resolved via the precomputed CSR list 
    bitset_copy(inter, &Dom[preds->predData[start]]); // first pred: copy
    for (int i = 1; i < preds->predCount[b]; i++) {
        int p = preds->predData[start + i];
        // subsequent preds: AND word by word (intersection)
        for (int w = 0; w < words; w++) {
            inter->bits[w] &= Dom[p].bits[w];
        }
    }
}

/**
 * @brief Compute candidate Dom[b] = {b} ∪ inter and update Dom[b] if it shrank.
 *
 * @return 1 if Dom[b] changed, 0 otherwise.
 */
static int loop_update_dominator_set(BitSet *Dom_b, const BitSet *inter, int b, BitSet *tmp) {
    // Dom[b] = {b} ∪ inter
    bitset_copy(tmp, inter);
    bitset_set(tmp, b);

    if (!bitset_equal(Dom_b, tmp)) {
        bitset_copy(Dom_b, tmp);
        return 1; // Dom set shrank: keep iterating
    }
    return 0;
}


BitSet *loop_compute_dominators(IRFunction *f, int words, Arena *arena) {
    int n = f->blockCount;
    BitSet *Dom = arena_alloc(arena, (size_t)n * sizeof(BitSet));

    // predecessor list built once, reused across every fixed-point iteration
    // (succ[] is read-only throughout this function: safe to build up front)
    PredList preds = ir_build_pred_list(f, arena);

    // initialise Dom sets
    loop_init_dominator_sets(Dom, n, words, arena);

    // scratch sets for the intersection step
    BitSet inter = bitset_new(arena, words);
    BitSet tmp   = bitset_new(arena, words);

    int changed = 1;
    while (changed) {
        changed = 0;
        // skip block 0: its Dom set is fixed to {0}
        for (int b = 1; b < n; b++) {
            if (preds.predCount[b] == 0) continue; // block unreachable: no predecessors found

            loop_intersect_predecessor_dominators(&inter, Dom, &preds, b, words);

            if (loop_update_dominator_set(&Dom[b], &inter, b, &tmp)) {
                changed = 1;
            }
        }
    }
    return Dom;
}

int loop_dominates(BitSet *Dom, int a, int b) {
    return bitset_test(&Dom[b], a);
}

/**
 * @brief Perform reverse BFS from @p tail up to @p header to discover body blocks.
 */
static void loop_bfs_traverse_loop_body(int header, int tail, char *inBody,
                                   int *queue, const PredList *preds) {
    int head = 0, tail_q = 0;

    // seed: both header and tail are always in the body
    inBody[header] = inBody[tail] = 1;
    queue[tail_q++] = tail;

    while (head < tail_q) {
        int b = queue[head++];
        // walk b's actual predecessors via the CSR list
        int start = preds->predStart[b];
        for (int k = 0; k < preds->predCount[b]; k++) {
            int p = preds->predData[start + k];
            if (!inBody[p]) {
                inBody[p] = 1;
                queue[tail_q++] = p;
            }
        }
    }
}

/**
 * @brief Gather all marked body blocks in block index order for deterministic output.
 */
static void gather_body_blocks_in_order(const char *inBody, int n, int *body, int *bodyCount) {
    *bodyCount = 0;
    for (int b = 0; b < n; b++) {
        if (inBody[b]) {
            body[(*bodyCount)++] = b;
        }
    }
}

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
static void loop_collect_body(IRFunction *f, int header, int tail,
                        int *body, int *bodyCount, const PredList *preds,
                        Arena *arena) {
    int n = f->blockCount;

    // membership flag for the BFS, arena-scratch, reset per call
    char *inBody = arena_alloc(arena, (size_t)n);
    memset(inBody, 0, (size_t)n);

    // BFS queue (arena-allocated; at most n entries)
    int *queue = arena_alloc(arena, (size_t)n * sizeof(int));

    loop_bfs_traverse_loop_body(header, tail, inBody, queue, preds);
    gather_body_blocks_in_order(inBody, n, body, bodyCount);
}

/* =========================================================================
 * Loop detection
 * ========================================================================= */

/**
 * @brief Scan body blocks and record exit blocks (having successors outside body).
 */
static void loop_record_loop_exit_blocks(Loop *L, const IRFunction *f,
                                    const char *inBody, const int *body, int bodyCount) {
    // find exit blocks: body blocks with at least one out-of-loop successor
    for (int i = 0; i < bodyCount && L->exitCount < 64; i++) {
        int bl = body[i];
        for (int s = 0; s < 2; s++) {
            int succ = f->blocks[bl].bb.succ[s];
            if (succ < 0 || inBody[succ]) continue; // not an exit edge

            // record bl as an exit block (at most once)
            int already = 0;
            for (int e = 0; e < L->exitCount; e++) {
                if (L->exits[e] == bl) {
                    already = 1;
                    break;
                }
            }
            if (!already) {
                L->exits[L->exitCount++] = bl;
            }
            break; // one out-of-loop successor is enough to classify bl as exit
        }
    }
}

/**
 * @brief Construct a natural loop instance from a verified back-edge h -> b.
 */
static void loop_build_natural_loop(Loop *L, IRFunction *f, int h, int b,
                               int *body, const PredList *preds,
                               Arena *arena, int n) {
    L->header    = h;
    L->preHeader = -1; // not yet created
    L->exitCount = 0;

    // collect body; copy into arena-allocated array owned by Loop
    int bodyCount = 0;
    loop_collect_body(f, h, b, body, &bodyCount, preds, arena);
    L->body      = arena_alloc(arena, (size_t)bodyCount * sizeof(int));
    L->bodyCount = bodyCount;
    memcpy(L->body, body, (size_t)bodyCount * sizeof(int));

    // mark body membership for O(1) exit-block test below
    char *inBody = arena_alloc(arena, (size_t)n);
    memset(inBody, 0, (size_t)n);
    for (int i = 0; i < bodyCount; i++) {
        inBody[body[i]] = 1;
    }

    loop_record_loop_exit_blocks(L, f, inBody, body, bodyCount);
}


int loop_find(IRFunction *f, BitSet *Dom, Loop *loops, Arena *arena) {
    int n = f->blockCount, nLoops = 0;
    // reuse a single body scratch buffer across all loops
    int *body = arena_alloc(arena, (size_t)n * sizeof(int));

    // predecessor list built once, reused by every loop_collect_body() call below
    // instead of each call reconstructing its own reverse-adjacency lists
    PredList preds = ir_build_pred_list(f, arena);

    for (int b = 0; b < n && nLoops < MAX_LOOPS; b++) {
        for (int k = 0; k < 2; k++) {
            int h = f->blocks[b].bb.succ[k];
            // back-edge: successor h dominates b
            if (h < 0 || !loop_dominates(Dom, h, b)) continue;

            Loop *L = &loops[nLoops++];
            loop_build_natural_loop(L, f, h, b, body, &preds, arena, n);
        }
    }
    return nLoops;
}

/* 
 * Pre-header insertion
 */

/**
 * @brief Ensure f->blocks array has capacity for adding at least one block.
 */
static void ensure_block_capacity(IRFunction *f) {
    if (f->blockCount == f->blockCap) {
        f->blockCap = f->blockCap ? f->blockCap * 2 : 16;
        f->blocks   = realloc(f->blocks, (size_t)f->blockCap * sizeof(IRBlock));
    }
}

/**
 * @brief Append an empty synthetic pre-header block to the IRFunction.
 */
static int loop_create_pre_header_block(IRFunction *f, int header) {
    ensure_block_capacity(f);

    int phIdx   = f->blockCount++;
    IRBlock *ph = &f->blocks[phIdx];

    // empty block: instructions will be inserted here by LICM/SR
    ph->bb.range.start = ph->bb.range.end = f->count;
    ph->bb.succ[0]     = header; // pre-header falls through to the loop header
    ph->bb.succ[1]     = -1;
    ph->predCount      = 0;

    return phIdx;
}

/**
 * @brief Re-route non-body predecessors of loop header to target the pre-header.
 */
static void loop_reroute_non_body_predecessors(IRFunction *f, int header, int phIdx, const Loop *L) {
    // build body membership for O(1) lookup during predecessor re-routing
    char *inBody = calloc((size_t)(phIdx + 1), 1);
    for (int i = 0; i < L->bodyCount; i++) {
        inBody[L->body[i]] = 1;
    }

    // re-route non-body predecessors of header through the pre-header
    for (int b = 0; b < phIdx; b++) {
        for (int k = 0; k < 2; k++) {
            if (f->blocks[b].bb.succ[k] == header && !inBody[b]) {
                f->blocks[b].bb.succ[k] = phIdx; // b now jumps to pre-header
                f->blocks[header].predCount--;    // header loses one predecessor
                f->blocks[phIdx].predCount++;    // pre-header gains one
            }
        }
    }

    free(inBody);
}


int loop_build_pre_header(IRFunction *f, Loop *L) {
    int header = L->header;

    int phIdx = loop_create_pre_header_block(f, header);

    loop_reroute_non_body_predecessors(f, header, phIdx, L);

    // header gains the pre-header as its new (only) non-back-edge predecessor
    f->blocks[header].predCount++;
    L->preHeader = phIdx;
    return phIdx;
}