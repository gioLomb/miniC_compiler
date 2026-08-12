#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "regalloc.h"
#include "liveness.h"
#include "interference.h"
#include "regalloc_utils.h"
#include "arena.h"

/* =========================================================================
 * Bitset helpers — usati da SpillSet per is_spilled O(1)
 * =========================================================================
 * Sostituisce la vecchia ricerca lineare is_spilled() O(nSpilled).
 * Dimensione massima: nextVreg virtual register, sempre < qualche migliaio.
 * ========================================================================= */

#define SPILLSET_WORDS(n) (((n) + 63) / 64)

typedef struct {
    uint64_t *bits;
    int       cap;   /* numero di words allocate */
} SpillSet;

static inline void spillset_init(SpillSet *s, int n) {
    s->cap  = SPILLSET_WORDS(n);
    s->bits = calloc((size_t)s->cap, sizeof(uint64_t));
}

static inline void spillset_free(SpillSet *s) {
    free(s->bits);
}

static inline void spillset_set(SpillSet *s, int v) {
    s->bits[v >> 6] |= 1ULL << (v & 63);
}

static inline int spillset_test(const SpillSet *s, int v) {
    return (int)((s->bits[v >> 6] >> (v & 63)) & 1ULL);
}

/* =========================================================================
 * CFG construction
 * ========================================================================= */

static int find_label_block(const int *labelIds, const int *blockIdx,
                             int n, int labelId) {
    for (int i = 0; i < n; i++)
        if (labelIds[i] == labelId) return blockIdx[i];
    return -1;
}

static BasicBlock *build_cfg(const MachFunction *f, int *outCount) {
    int cap = 8, count = 0;
    BasicBlock *blocks = malloc((size_t)cap * sizeof(BasicBlock));
    int start = 0;

    for (int i = 0; i < f->count; i++) {
        if (f->instrs[i].op == MACH_LABEL && i > start) {
            if (count == cap) {
                cap *= 2;
                blocks = realloc(blocks, (size_t)cap * sizeof(BasicBlock));
            }
            blocks[count].start    = start;
            blocks[count].end      = i;
            blocks[count].succ[0]  = blocks[count].succ[1] = -1;
            count++;
            start = i;
        }
    }
    if (start < f->count) {
        if (count == cap) {
            cap *= 2;
            blocks = realloc(blocks, (size_t)cap * sizeof(BasicBlock));
        }
        blocks[count].start    = start;
        blocks[count].end      = f->count;
        blocks[count].succ[0]  = blocks[count].succ[1] = -1;
        count++;
    }

    int *labelIds = malloc((size_t)(count ? count : 1) * sizeof(int));
    int *blockIdx = malloc((size_t)(count ? count : 1) * sizeof(int));
    int nLabels = 0;

    for (int b = 0; b < count; b++) {
        if (f->instrs[blocks[b].start].op == MACH_LABEL) {
            labelIds[nLabels] = f->instrs[blocks[b].start].dst.labelId;
            blockIdx[nLabels] = b;
            nLabels++;
        }
    }

    for (int b = 0; b < count; b++) {
        blocks[b].succ[0] = blocks[b].succ[1] = -1;
        int  last = blocks[b].end - 1;
        MachOp op = f->instrs[last].op;
        switch (op) {
        case MACH_JMP:
            blocks[b].succ[0] = find_label_block(labelIds, blockIdx, nLabels,
                                                  f->instrs[last].dst.labelId);
            break;
        case MACH_JE: case MACH_JNE: case MACH_JL:
        case MACH_JLE: case MACH_JG: case MACH_JGE:
            blocks[b].succ[0] = (b + 1 < count) ? b + 1 : -1;
            blocks[b].succ[1] = find_label_block(labelIds, blockIdx, nLabels,
                                                  f->instrs[last].dst.labelId);
            break;
        case MACH_RET:
            break;
        default:
            blocks[b].succ[0] = (b + 1 < count) ? b + 1 : -1;
        }
    }

    free(labelIds);
    free(blockIdx);
    *outCount = count;
    return blocks;
}

/* =========================================================================
 * Buckets for Simplify (Briggs-optimistic)
 *
 * OPT: aggiunto campo 'nonempty' (bitmask uint32_t, k<=14 bucket) per
 * rendere bucket_pop_any_low O(1) con __builtin_ctz invece di O(k).
 * ========================================================================= */

typedef struct {
    int     *head;
    int     *bnext;
    int     *bprev;
    int     *inBucket;
    int      k;
    uint32_t nonempty;   /* bit i settato ↔ head[i] != -1 */
} Buckets;

static Buckets buckets_create(int nextVreg, int k) {
    Buckets b;
    b.k        = k;
    b.nonempty = 0;
    b.head     = malloc((size_t)k * sizeof(int));
    for (int d = 0; d < k; d++) b.head[d] = -1;
    b.bnext    = malloc((size_t)(nextVreg ? nextVreg : 1) * sizeof(int));
    b.bprev    = malloc((size_t)(nextVreg ? nextVreg : 1) * sizeof(int));
    b.inBucket = calloc((size_t)(nextVreg ? nextVreg : 1), sizeof(int));
    return b;
}

static void buckets_free(Buckets *b) {
    free(b->head);
    free(b->bnext);
    free(b->bprev);
    free(b->inBucket);
}

static inline void bucket_insert(Buckets *b, int v, int d) {
    b->bprev[v]  = -1;
    b->bnext[v]  = b->head[d];
    if (b->head[d] >= 0) b->bprev[b->head[d]] = v;
    b->head[d]   = v;
    b->inBucket[v] = 1;
    b->nonempty |= (1u << d);
}

static inline void bucket_remove(Buckets *b, int v, int d) {
    if (b->bprev[v] >= 0) b->bnext[b->bprev[v]] = b->bnext[v];
    else                  b->head[d]             = b->bnext[v];
    if (b->bnext[v] >= 0) b->bprev[b->bnext[v]] = b->bprev[v];
    b->inBucket[v] = 0;
    if (b->head[d] < 0) b->nonempty &= ~(1u << d);
}

/* OPT: O(1) con __builtin_ctz sulla bitmask nonempty */
static inline int bucket_pop_any_low(Buckets *b, int *outD) {
    if (!b->nonempty) return -1;
    *outD = __builtin_ctz(b->nonempty);
    return b->head[*outD];
}

/* =========================================================================
 * Simplify (Briggs-optimistic)
 * ========================================================================= */

static int simplify(IGraph *g, int nextVreg, int **outStack) {
    *outStack = malloc((size_t)(nextVreg ? nextVreg : 1) * sizeof(int));
    int stackLen  = 0;
    int k         = PHYS_ALLOCATABLE;
    Buckets buckets = buckets_create(nextVreg, k);
    int remaining = nextVreg;

    for (int v = 0; v < nextVreg; v++)
        if (g->degree[v] < k)
            bucket_insert(&buckets, v, g->degree[v]);

    while (remaining > 0) {
        int d;
        int chosen = bucket_pop_any_low(&buckets, &d);

        if (chosen < 0) {
            /* nessun nodo con grado < k: scegli candidato spill minimo cost/degree */
            double best = 1e18;
            for (int v = 0; v < nextVreg; v++) {
                if (!g->active[v] || buckets.inBucket[v]) continue;
                double ratio = g->degree[v] > 0
                               ? (double)g->spillCost[v] / g->degree[v]
                               : 0.0;
                if (ratio < best) { best = ratio; chosen = v; }
            }
            if (chosen < 0) break;
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
 * Select colors
 *
 * OPT: scelta colore con __builtin_ctz(~forbidden) invece di loop O(k).
 * ========================================================================= */

static int select_colors(IGraph *g, int nextVreg, int *stack,
                          int stackLen, int *spilled) {
    (void)nextVreg;
    int nSpilled = 0;

    for (int si = stackLen - 1; si >= 0; si--) {
        int v = stack[si];
        g->active[v] = 1;

        uint32_t forbidden = g->excl[v];
        for (int k = 0; k < g->adj[v].len; k++) {
            int w = g->adj[v].data[k];
            if (g->color[w] >= 0)
                forbidden |= (1u << g->color[w]);
        }

        /* maschera bit validi: solo PHYS_ALLOCATABLE colori disponibili */
        const uint32_t valid_mask = (1u << PHYS_ALLOCATABLE) - 1u;
        uint32_t available = (~forbidden) & valid_mask;

        int chosen = -1;
        if (g->crossesCall[v]) {
            /* preferisci callee-saved: bit [PHYS_CALLER_SAVED_COUNT .. PHYS_ALLOCATABLE-1] */
            uint32_t callee_avail = available >> PHYS_CALLER_SAVED_COUNT;
            if (callee_avail)
                chosen = PHYS_CALLER_SAVED_COUNT + __builtin_ctz(callee_avail);
            else if (available)
                chosen = __builtin_ctz(available);
        } else {
            if (available)
                chosen = __builtin_ctz(available);
        }

        if (chosen >= 0) g->color[v] = chosen;
        else {
            g->color[v]      = -2;
            spilled[nSpilled++] = v;
        }
    }

    return nSpilled;
}

/* =========================================================================
 * Spill code insertion
 * ========================================================================= */

/* OPT: memset per -1 (0xFF byte valido per int -1 in two's complement) */
static inline void invalidate_cache(int *cache, int n) {
    memset(cache, 0xFF, (size_t)n * sizeof(int));
}

static void load_spilled(MachOperand *o, int origVreg, MachFunction *f,
                          int off, MachInstr *newInstrs, int *newCount,
                          int *cache) {
    if (cache[origVreg] >= 0) {
        o->kind   = MO_VREG;
        o->vregId = cache[origVreg];
        return;
    }
    int tmp = f->nextVreg++;
    MachInstr ld      = {0};
    ld.op             = MACH_MOV;
    ld.dst.kind       = MO_VREG;  ld.dst.vregId   = tmp;
    ld.src1.kind      = MO_STACK; ld.src1.stackOff = off;
    ld.src2.kind      = MO_NONE;
    newInstrs[(*newCount)++] = ld;
    o->kind   = MO_VREG;
    o->vregId = tmp;
    cache[origVreg] = tmp;
}

static void spill_insert(MachFunction *f, const int *spilled, int nSpilled,
                          int *frameOff) {
    int origNextVreg = f->nextVreg;

    /* slot[v] = offset stack per il vreg spillato v; -1 se non spillato */
    int *slot = malloc((size_t)origNextVreg * sizeof(int));
    for (int i = 0; i < origNextVreg; i++) slot[i] = -1;
    for (int i = 0; i < nSpilled; i++) {
        *frameOff      += 8;
        slot[spilled[i]] = *frameOff;
    }

    /* OPT: SpillSet O(1) invece di is_spilled() O(nSpilled) */
    SpillSet ss;
    spillset_init(&ss, origNextVreg);
    for (int i = 0; i < nSpilled; i++) spillset_set(&ss, spilled[i]);

    int *cache = malloc((size_t)origNextVreg * sizeof(int));
    invalidate_cache(cache, origNextVreg);

    int maxNew        = f->count * 3 + 16;
    MachInstr *newInstrs = malloc((size_t)maxNew * sizeof(MachInstr));
    int newCount      = 0;

    for (int i = 0; i < f->count; i++) {
        MachInstr in      = f->instrs[i];
        int isStore       = (in.op == MACH_STORE);

        if (in.op == MACH_LABEL) invalidate_cache(cache, origNextVreg);

        /* ricarica sorgenti spillati */
        MachOperand *srcs[2] = { &in.src1, &in.src2 };
        for (int s = 0; s < 2; s++) {
            if (srcs[s]->kind == MO_VREG
                && srcs[s]->vregId < origNextVreg
                && spillset_test(&ss, srcs[s]->vregId)) {
                int orig = srcs[s]->vregId;
                load_spilled(srcs[s], orig, f, slot[orig],
                             newInstrs, &newCount, cache);
            }
        }

        /* base/index di MO_MEM */
        MachOperand *memHolder = isStore ? &in.dst : &in.src1;
        if (memHolder->kind == MO_MEM) {
            if (memHolder->mem.baseVreg >= 0
                && memHolder->mem.baseVreg < origNextVreg
                && spillset_test(&ss, memHolder->mem.baseVreg)) {
                int orig = memHolder->mem.baseVreg;
                MachOperand tmpOp; tmpOp.kind = MO_VREG; tmpOp.vregId = orig;
                load_spilled(&tmpOp, orig, f, slot[orig],
                             newInstrs, &newCount, cache);
                memHolder->mem.baseVreg = tmpOp.vregId;
            }
            if (memHolder->mem.indexVreg >= 0
                && memHolder->mem.indexVreg < origNextVreg
                && spillset_test(&ss, memHolder->mem.indexVreg)) {
                int orig = memHolder->mem.indexVreg;
                MachOperand tmpOp; tmpOp.kind = MO_VREG; tmpOp.vregId = orig;
                load_spilled(&tmpOp, orig, f, slot[orig],
                             newInstrs, &newCount, cache);
                memHolder->mem.indexVreg = tmpOp.vregId;
            }
        }

        /* destinazione spillata */
        int dstSpilled   = (in.dst.kind == MO_VREG) && !isStore
                           && in.dst.vregId < origNextVreg
                           && spillset_test(&ss, in.dst.vregId);
        int dstStoreOff  = -1, origDstVreg = -1;

        if (dstSpilled) {
            origDstVreg = in.dst.vregId;
            dstStoreOff = slot[origDstVreg];
            int dstTmp;
            if (regalloc_is_rmw(in.op)) {
                MachOperand tmpOp; tmpOp.kind = MO_VREG; tmpOp.vregId = origDstVreg;
                load_spilled(&tmpOp, origDstVreg, f, dstStoreOff,
                             newInstrs, &newCount, cache);
                dstTmp = tmpOp.vregId;
            } else {
                dstTmp = f->nextVreg++;
            }
            in.dst.vregId = dstTmp;
        }

        newInstrs[newCount++] = in;

        if (dstSpilled) {
            MachInstr st   = {0};
            st.op          = MACH_MOV;
            st.dst.kind    = MO_STACK; st.dst.stackOff  = dstStoreOff;
            st.src1.kind   = MO_VREG;  st.src1.vregId   = in.dst.vregId;
            st.src2.kind   = MO_NONE;
            newInstrs[newCount++] = st;
            cache[origDstVreg]   = in.dst.vregId;
        }

        if (regalloc_is_ctrl_transfer(in.op)) invalidate_cache(cache, origNextVreg);
    }

    free(f->instrs);
    f->instrs   = newInstrs;
    f->count    = newCount;
    f->capacity = maxNew;
    free(slot);
    free(cache);
    spillset_free(&ss);
}

/* =========================================================================
 * Finalize + remove identity moves — fusi in un unico passaggio
 *
 * OPT: prima erano due loop separati su f->instrs; ora uno solo.
 * ========================================================================= */

static inline void rewrite_phys(MachOperand *o, const int *color) {
    if (o->kind == MO_VREG) {
        o->kind    = MO_PHYS;
        o->physReg = color[o->vregId];
    }
}

static inline void rewrite_mem_operands(MachOperand *op, const int *color) {
    if (op->kind != MO_MEM) return;
    if (op->mem.baseVreg  >= 0) op->mem.baseVreg  = color[op->mem.baseVreg];
    if (op->mem.indexVreg >= 0) op->mem.indexVreg = color[op->mem.indexVreg];
}

/* Colora tutti i vreg e scarta i MOV identità in un unico passaggio */
static void finalize_and_remove_identity(MachFunction *f, const int *color) {
    int newCount = 0;
    for (int i = 0; i < f->count; i++) {
        MachInstr *in = &f->instrs[i];

        rewrite_phys(&in->dst,  color);
        rewrite_phys(&in->src1, color);
        rewrite_phys(&in->src2, color);
        rewrite_mem_operands(&in->dst,  color);
        rewrite_mem_operands(&in->src1, color);

        /* salta MOV reg→stesso reg */
        if (in->op == MACH_MOV
            && in->dst.kind  == MO_PHYS
            && in->src1.kind == MO_PHYS
            && in->dst.physReg == in->src1.physReg)
            continue;

        f->instrs[newCount++] = *in;
    }
    f->count = newCount;
}

/* =========================================================================
 * Save/restore callee-saved
 *
 * OPT: eliminato count_rets() separato; conta i RET durante la scansione
 * per usedMask, evitando un secondo giro completo sulla lista istruzioni.
 * ========================================================================= */

static inline int is_callee_saved(int physReg) {
    return physReg >= PHYS_RBX && physReg <= PHYS_R15;
}

static void save_restore_callee(MachFunction *f) {
    uint32_t usedMask = 0;
    int retCount      = 0;

    /* unico passaggio: raccoglie callee-saved usati E conta RET */
    for (int i = 0; i < f->count; i++) {
        MachInstr *in = &f->instrs[i];
        if (in->dst.kind == MO_PHYS && is_callee_saved(in->dst.physReg))
            usedMask |= (1u << in->dst.physReg);
        if (in->op == MACH_RET) retCount++;
    }

    if (usedMask == 0) return;

    int used[PHYS_CALLEE_SAVED_COUNT], usedCount = 0;
    for (int p = PHYS_RBX; p <= PHYS_R15; p++)
        if ((usedMask >> p) & 1u) used[usedCount++] = p;

    int funcBeginIdx = -1;
    for (int i = 0; i < f->count; i++)
        if (f->instrs[i].op == MACH_FUNC_BEGIN) { funcBeginIdx = i; break; }

    MachInstr *newInstrs = malloc(
        (size_t)(f->count + usedCount + (size_t)usedCount * retCount) * sizeof(MachInstr));
    int newCount = 0;

    for (int i = 0; i < f->count; i++) {
        if (i == funcBeginIdx) {
            newInstrs[newCount++] = f->instrs[i];
            for (int k = 0; k < usedCount; k++) {
                MachInstr ps = {0};
                ps.op         = MACH_PUSH;
                ps.dst.kind   = MO_PHYS; ps.dst.physReg = used[k];
                ps.src1.kind  = MO_NONE; ps.src2.kind   = MO_NONE;
                newInstrs[newCount++] = ps;
            }
            continue;
        }
        if (f->instrs[i].op == MACH_RET) {
            for (int k = usedCount - 1; k >= 0; k--) {
                MachInstr po = {0};
                po.op         = MACH_POP;
                po.dst.kind   = MO_PHYS; po.dst.physReg = used[k];
                po.src1.kind  = MO_NONE; po.src2.kind   = MO_NONE;
                newInstrs[newCount++] = po;
            }
        }
        newInstrs[newCount++] = f->instrs[i];
    }

    free(f->instrs);
    f->instrs = newInstrs;
    f->count  = newCount;
}

/* =========================================================================
 * Main register allocation loop
 * ========================================================================= */

static void regalloc_function(MachFunction *f) {
    int frameOff = 0;

    for (;;) {
        int nBlocks;
        BasicBlock *blocks = build_cfg(f, &nBlocks);

        Arena *livArena = arena_create(0);
        LivenessResult liv = liveness_compute_mach(f, blocks, nBlocks, livArena);

        IGraph g = ig_build(f, blocks, nBlocks, f->nextVreg, liv.liveAfter,livArena);

        int *stack    = NULL;
        int  stackLen = simplify(&g, f->nextVreg, &stack);

        int *spilled  = malloc((size_t)(f->nextVreg ? f->nextVreg : 1) * sizeof(int));
        int  nSpilled = select_colors(&g, f->nextVreg, stack, stackLen, spilled);

        if (nSpilled == 0) {
            /* OPT: finalize + remove identity in un unico passaggio */
            finalize_and_remove_identity(f, g.color);
            free(stack); free(spilled); ig_free(&g);
            arena_destroy(livArena); free(blocks);
            break;
        }

        spill_insert(f, spilled, nSpilled, &frameOff);
        free(stack); free(spilled); ig_free(&g);
        arena_destroy(livArena); free(blocks);
    }

    save_restore_callee(f);
    f->frameSize = (frameOff + 15) & ~15;
}

void regalloc(MachProgram *mp) {
    for (int i = 0; i < mp->count; i++)
        regalloc_function(mp->functions[i]);
}