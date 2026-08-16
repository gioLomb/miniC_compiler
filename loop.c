#include <stdlib.h>
#include <string.h>
#include "loop.h"

/* ---- Dominatori -------------------------------------------------------- */

LiveSet *loop_compute_dominators(IRFunction *f, int words, Arena *arena) {
    int n = f->blockCount;
    LiveSet *Dom = arena_alloc(arena, (size_t)n * sizeof(LiveSet));

    for (int b = 0; b < n; b++) {
        Dom[b] = liveset_new(arena, words);
        if (b == 0) {
            liveset_set(&Dom[b], 0);
        } else {
            memset(Dom[b].bits, 0xFF, (size_t)words * sizeof(uint64_t));
            int leftover = n % 64;
            if (leftover) Dom[b].bits[words - 1] = (1ULL << leftover) - 1;
        }
    }

    LiveSet inter = liveset_new(arena, words);
    LiveSet tmp   = liveset_new(arena, words);
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int b = 1; b < n; b++) {
            int firstPred = 1;
            for (int p = 0; p < n; p++) {
                for (int k = 0; k < 2; k++) {
                    if (f->blocks[p].bb.succ[k] != b) continue;
                    if (firstPred) { liveset_copy(&inter, &Dom[p]); firstPred = 0; }
                    else {
                        for (int w = 0; w < words; w++)
                            inter.bits[w] &= Dom[p].bits[w];
                    }
                }
            }
            if (firstPred) continue;
            liveset_copy(&tmp, &inter);
            liveset_set(&tmp, b);
            if (!liveset_equal(&Dom[b], &tmp)) {
                liveset_copy(&Dom[b], &tmp);
                changed = 1;
            }
        }
    }
    return Dom;
}

int loop_dominates(LiveSet *Dom, int a, int b) {
    return liveset_test(&Dom[b], a);
}

/* ---- Rilevamento loop -------------------------------------------------- */

static void collectBody(IRFunction *f, int header, int tail,
                        int *body, int *bodyCount, Arena *arena) {
    int n = f->blockCount;
    char *inBody    = arena_alloc(arena, (size_t)n);
    int  *predCount = arena_alloc(arena, (size_t)n * sizeof(int));
    int (*preds)[2] = arena_alloc(arena, (size_t)n * 2 * sizeof(int));
    memset(inBody,    0, (size_t)n);
    memset(predCount, 0, (size_t)n * sizeof(int));
    memset(preds,    -1, (size_t)n * 2 * sizeof(int));
    for (int b = 0; b < n; b++)
        for (int k = 0; k < 2; k++) {
            int s = f->blocks[b].bb.succ[k];
            if (s >= 0) preds[s][predCount[s]++] = b;
        }

    int *queue = arena_alloc(arena, (size_t)n * sizeof(int));
    int head = 0, tail_q = 0;
    inBody[header] = inBody[tail] = 1;
    queue[tail_q++] = tail;
    while (head < tail_q) {
        int b = queue[head++];
        for (int k = 0; k < predCount[b]; k++) {
            int p = preds[b][k];
            if (p >= 0 && !inBody[p]) { inBody[p] = 1; queue[tail_q++] = p; }
        }
    }
    *bodyCount = 0;
    for (int b = 0; b < n; b++) if (inBody[b]) body[(*bodyCount)++] = b;
}

int loop_find(IRFunction *f, LiveSet *Dom, Loop *loops, Arena *arena) {
    int n = f->blockCount, nLoops = 0;
    int *body = arena_alloc(arena, (size_t)n * sizeof(int));

    for (int b = 0; b < n && nLoops < MAX_LOOPS; b++) {
        for (int k = 0; k < 2; k++) {
            int h = f->blocks[b].bb.succ[k];
            if (h < 0 || !loop_dominates(Dom, h, b)) continue;

            Loop *L = &loops[nLoops++];
            L->header = h; L->preHeader = -1; L->exitCount = 0;

            int bodyCount = 0;
            collectBody(f, h, b, body, &bodyCount, arena);
            L->body = arena_alloc(arena, (size_t)bodyCount * sizeof(int));
            L->bodyCount = bodyCount;
            memcpy(L->body, body, (size_t)bodyCount * sizeof(int));

            char *inBody = arena_alloc(arena, (size_t)n);
            memset(inBody, 0, (size_t)n);
            for (int i = 0; i < bodyCount; i++) inBody[body[i]] = 1;

            for (int i = 0; i < bodyCount && L->exitCount < 64; i++) {
                int bl = body[i];
                for (int s = 0; s < 2; s++) {
                    int succ = f->blocks[bl].bb.succ[s];
                    if (succ < 0 || inBody[succ]) continue;
                    int already = 0;
                    for (int e = 0; e < L->exitCount; e++)
                        if (L->exits[e] == bl) { already = 1; break; }
                    if (!already) L->exits[L->exitCount++] = bl;
                    break;
                }
            }
        }
    }
    return nLoops;
}

/* ---- Pre-header -------------------------------------------------------- */

int loop_build_pre_header(IRFunction *f, Loop *L) {
    int header = L->header;
    if (f->blockCount == f->blockCap) {
        f->blockCap = f->blockCap ? f->blockCap * 2 : 16;
        f->blocks = realloc(f->blocks, (size_t)f->blockCap * sizeof(IRBlock));
    }
    int phIdx    = f->blockCount++;
    IRBlock *ph  = &f->blocks[phIdx];
    ph->bb.start    = ph->bb.end = f->count;
    ph->bb.succ[0]  = header;
    ph->bb.succ[1]  = -1;
    ph->predCount   = 0;

    char *inBody = calloc((size_t)(phIdx + 1), 1);
    for (int i = 0; i < L->bodyCount; i++) inBody[L->body[i]] = 1;

    for (int b = 0; b < phIdx; b++)
        for (int k = 0; k < 2; k++)
            if (f->blocks[b].bb.succ[k] == header && !inBody[b]) {
                f->blocks[b].bb.succ[k] = phIdx;
                f->blocks[header].predCount--;
                ph->predCount++;
            }
    free(inBody);

    f->blocks[header].predCount++;
    L->preHeader = phIdx;
    return phIdx;
}
