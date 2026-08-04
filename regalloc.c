#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "regalloc.h"

/* =========================================================================
 * Bitset generico (malloc: il loop esterno di spill itera piu' volte).
 * ========================================================================= */
typedef struct { uint64_t *bits; int words; } RSet;

static RSet rset_new(int n) {
    RSet s; s.words = (n + 63) / 64;
    s.bits = calloc((size_t)s.words, sizeof(uint64_t));
    return s;
}
static void rset_free(RSet *s) { free(s->bits); s->bits = NULL; }
static void rset_clear(RSet *s) { memset(s->bits, 0, (size_t)s->words * sizeof(uint64_t)); }
static void rset_set(RSet *s, int id) { s->bits[id >> 6] |= 1ULL << (id & 63); }
static void rset_clr(RSet *s, int id) { s->bits[id >> 6] &= ~(1ULL << (id & 63)); }
static int  rset_test(const RSet *s, int id) { return (int)((s->bits[id >> 6] >> (id & 63)) & 1ULL); }
static void rset_copy(RSet *d, const RSet *s) { memcpy(d->bits, s->bits, (size_t)s->words * sizeof(uint64_t)); }
static void rset_union(RSet *d, const RSet *s) { for (int i = 0; i < d->words; i++) d->bits[i] |= s->bits[i]; }
static void rset_diff(RSet *d, const RSet *a, const RSet *b) { for (int i = 0; i < d->words; i++) d->bits[i] = a->bits[i] & ~b->bits[i]; }
static int  rset_equal(const RSet *a, const RSet *b) { for (int i = 0; i < a->words; i++) if (a->bits[i] != b->bits[i]) return 0; return 1; }

/* Itera solo i bit settati (word-scan + ctz): O(popcount) invece di O(n) */
#define RSET_FOREACH(s, idvar) \
    for (int _w = 0; _w < (s)->words; _w++) { \
        uint64_t _bits = (s)->bits[_w]; \
        while (_bits) { \
            int _b = __builtin_ctzll(_bits); \
            int idvar = (_w << 6) + _b; \
            _bits &= (_bits - 1);
#define RSET_FOREACH_END } }

/* =========================================================================
 * Blocchi base + CFG locale per MachFunction.
 * ========================================================================= */
typedef struct { int start, end, succ[2]; } RBlock;
typedef struct { int labelId, blockIdx; } LabelEntry;

static int find_label_block(LabelEntry *tbl, int n, int labelId) {
    for (int i = 0; i < n; i++) if (tbl[i].labelId == labelId) return tbl[i].blockIdx;
    return -1;
}

static RBlock *build_cfg(const MachFunction *f, int *outCount) {
    int cap = 8, count = 0;
    RBlock *blocks = malloc((size_t)cap * sizeof(RBlock));
    int start = 0;
    for (int i = 0; i < f->count; i++) {
        if (f->instrs[i].op == MACH_LABEL && i > start) {
            if (count == cap) { cap *= 2; blocks = realloc(blocks, (size_t)cap * sizeof(RBlock)); }
            blocks[count].start = start; blocks[count].end = i; count++;
            start = i;
        }
    }
    if (start < f->count) {
        if (count == cap) { cap *= 2; blocks = realloc(blocks, (size_t)cap * sizeof(RBlock)); }
        blocks[count].start = start; blocks[count].end = f->count; count++;
    }

    LabelEntry *labels = malloc((size_t)(count ? count : 1) * sizeof(LabelEntry));
    int nLabels = 0;
    for (int b = 0; b < count; b++) {
        if (f->instrs[blocks[b].start].op == MACH_LABEL) {
            labels[nLabels].labelId = f->instrs[blocks[b].start].dst.labelId;
            labels[nLabels].blockIdx = b;
            nLabels++;
        }
    }

    for (int b = 0; b < count; b++) {
        blocks[b].succ[0] = blocks[b].succ[1] = -1;
        int last = blocks[b].end - 1;
        MachOp op = f->instrs[last].op;
        switch (op) {
        case MACH_JMP:
            blocks[b].succ[0] = find_label_block(labels, nLabels, f->instrs[last].dst.labelId);
            break;
        case MACH_JE: case MACH_JNE: case MACH_JL:
        case MACH_JLE: case MACH_JG: case MACH_JGE:
            blocks[b].succ[0] = (b + 1 < count) ? b + 1 : -1;
            blocks[b].succ[1] = find_label_block(labels, nLabels, f->instrs[last].dst.labelId);
            break;
        case MACH_RET:
            break;
        default:
            blocks[b].succ[0] = (b + 1 < count) ? b + 1 : -1;
        }
    }
    free(labels);
    *outCount = count;
    return blocks;
}

/* =========================================================================
 * Classificazione def/use (spazio nodi: vreg [0,nextVreg) + fisici
 * [nextVreg, nextVreg+14)).
 * ========================================================================= */
static int normalize_phys(int p) { return p == PHYS_AL ? PHYS_RAX : p; }

static int node_of(const MachOperand *o, int nextVreg) {
    if (o->kind == MO_VREG) return o->vregId;
    if (o->kind == MO_PHYS) {
        int p = normalize_phys(o->physReg);
        if (p < PHYS_ALLOCATABLE) return nextVreg + p;
    }
    return -1;
}
static void mem_nodes(const MachOperand *o, int out[], int *n) {
    if (o->kind != MO_MEM) return;
    if (o->mem.baseVreg  >= 0) out[(*n)++] = o->mem.baseVreg;
    if (o->mem.indexVreg >= 0) out[(*n)++] = o->mem.indexVreg;
}
static int is_rmw(MachOp op) {
    switch (op) {
    case MACH_ADD: case MACH_SUB: case MACH_IMUL:
    case MACH_SAL: case MACH_NEG: case MACH_NOT: case MACH_XOR:
        return 1;
    default: return 0;
    }
}
static int is_setcc(MachOp op) {
    switch (op) {
    case MACH_SETE: case MACH_SETNE: case MACH_SETL:
    case MACH_SETLE: case MACH_SETG: case MACH_SETGE:
        return 1;
    default: return 0;
    }
}
static int is_ctrl_transfer(MachOp op) {
    switch (op) {
    case MACH_JMP: case MACH_JE: case MACH_JNE: case MACH_JL:
    case MACH_JLE: case MACH_JG: case MACH_JGE:
    case MACH_CALL: case MACH_RET:
        return 1;
    default: return 0;
    }
}

static void instr_defs(const MachInstr *in, int nextVreg, int out[], int *n) {
    *n = 0;
    switch (in->op) {
    case MACH_CMP: case MACH_TEST: case MACH_STORE: case MACH_PUSH:
    case MACH_JMP: case MACH_JE: case MACH_JNE: case MACH_JL: case MACH_JLE:
    case MACH_JG: case MACH_JGE: case MACH_CALL: case MACH_RET:
    case MACH_LABEL: case MACH_FUNC_BEGIN: case MACH_FUNC_END:
    case MACH_IDIV: case MACH_CQO:
        break;
    default: {
        int id = node_of(&in->dst, nextVreg);
        if (id >= 0) out[(*n)++] = id;
    }
    }
}

static void instr_uses(const MachInstr *in, int nextVreg, int out[], int *n) {
    *n = 0;
    int id;
    switch (in->op) {
    case MACH_STORE:
        mem_nodes(&in->dst, out, n);
        id = node_of(&in->src1, nextVreg); if (id >= 0) out[(*n)++] = id;
        break;
    case MACH_LOAD:
        mem_nodes(&in->src1, out, n);
        break;
    case MACH_IDIV:
    case MACH_PUSH:
        id = node_of(&in->dst, nextVreg); if (id >= 0) out[(*n)++] = id;
        break;
    case MACH_CQO:
        break;
    default:
        id = node_of(&in->src1, nextVreg); if (id >= 0) out[(*n)++] = id;
        id = node_of(&in->src2, nextVreg); if (id >= 0) out[(*n)++] = id;
        if (is_rmw(in->op)) {
            id = node_of(&in->dst, nextVreg); if (id >= 0) out[(*n)++] = id;
        }
    }
}

/* NOTA: CALL include uso implicito di tutti i caller-saved (oltre alla
 * def) — protegge i MOV che caricano gli argomenti nei registri fisici
 * prima della call: senza questo uso, la liveness non li propaga fino
 * alla CALL, niente arco verso quei registri, un vreg concorrente puo'
 * ricevere lo stesso colore e sporcare l'argomento in transito. RET usa
 * RAX (valore di ritorno), difensivo anche se RET e' sempre ultima istr. */
static void implicit_defs(const MachInstr *in, int nextVreg, int out[], int *n) {
    *n = 0;
    switch (in->op) {
    case MACH_IDIV: out[(*n)++] = nextVreg + PHYS_RAX; out[(*n)++] = nextVreg + PHYS_RDX; break;
    case MACH_CQO:  out[(*n)++] = nextVreg + PHYS_RDX; break;
    case MACH_CALL:
        for (int p = 0; p < PHYS_CALLER_SAVED_COUNT; p++) out[(*n)++] = nextVreg + p;
        break;
    default: break;
    }
}
static void implicit_uses(const MachInstr *in, int nextVreg, int out[], int *n) {
    *n = 0;
    switch (in->op) {
    case MACH_IDIV: out[(*n)++] = nextVreg + PHYS_RAX; out[(*n)++] = nextVreg + PHYS_RDX; break;
    case MACH_CQO:  out[(*n)++] = nextVreg + PHYS_RAX; break;
    case MACH_CALL:
        for (int p = 0; p < PHYS_CALLER_SAVED_COUNT; p++) out[(*n)++] = nextVreg + p;
        break;
    case MACH_RET:  out[(*n)++] = nextVreg + PHYS_RAX; break;
    default: break;
    }
}

/* =========================================================================
 * Fase 1: Liveness sulle MachInstr.
 * ========================================================================= */
typedef struct { RSet *liveAfter; int N, words; } MLiveness;

static MLiveness compute_liveness(const MachFunction *f, RBlock *blocks, int nBlocks, int nextVreg) {
    int N = nextVreg + PHYS_ALLOCATABLE;
    MLiveness r; r.N = N; r.words = (N + 63) / 64;

    RSet *use = malloc((size_t)nBlocks * sizeof(RSet));
    RSet *def = malloc((size_t)nBlocks * sizeof(RSet));
    RSet *liveIn  = malloc((size_t)nBlocks * sizeof(RSet));
    RSet *liveOut = malloc((size_t)nBlocks * sizeof(RSet));
    for (int b = 0; b < nBlocks; b++) {
        use[b] = rset_new(N); def[b] = rset_new(N);
        liveIn[b] = rset_new(N); liveOut[b] = rset_new(N);
    }

    int tmpArr[16];
    for (int b = 0; b < nBlocks; b++) {
        for (int i = blocks[b].start; i < blocks[b].end; i++) {
            const MachInstr *in = &f->instrs[i];
            int n;
            instr_uses(in, nextVreg, tmpArr, &n);
            for (int k = 0; k < n; k++) if (!rset_test(&def[b], tmpArr[k])) rset_set(&use[b], tmpArr[k]);
            implicit_uses(in, nextVreg, tmpArr, &n);
            for (int k = 0; k < n; k++) if (!rset_test(&def[b], tmpArr[k])) rset_set(&use[b], tmpArr[k]);
            instr_defs(in, nextVreg, tmpArr, &n);
            for (int k = 0; k < n; k++) rset_set(&def[b], tmpArr[k]);
            implicit_defs(in, nextVreg, tmpArr, &n);
            for (int k = 0; k < n; k++) rset_set(&def[b], tmpArr[k]);
        }
    }

    RSet tmp = rset_new(N);
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int b = nBlocks - 1; b >= 0; b--) {
            rset_clear(&liveOut[b]);
            for (int k = 0; k < 2; k++)
                if (blocks[b].succ[k] >= 0) rset_union(&liveOut[b], &liveIn[blocks[b].succ[k]]);
            rset_diff(&tmp, &liveOut[b], &def[b]);
            rset_union(&tmp, &use[b]);
            if (!rset_equal(&liveIn[b], &tmp)) { rset_copy(&liveIn[b], &tmp); changed = 1; }
        }
    }

    r.liveAfter = malloc((size_t)f->count * sizeof(RSet));
    for (int i = 0; i < f->count; i++) r.liveAfter[i] = rset_new(N);

    for (int b = 0; b < nBlocks; b++) {
        RSet live = rset_new(N);
        rset_copy(&live, &liveOut[b]);
        for (int i = blocks[b].end - 1; i >= blocks[b].start; i--) {
            rset_copy(&r.liveAfter[i], &live);
            const MachInstr *in = &f->instrs[i];
            int n;
            instr_defs(in, nextVreg, tmpArr, &n);
            for (int k = 0; k < n; k++) rset_clr(&live, tmpArr[k]);
            implicit_defs(in, nextVreg, tmpArr, &n);
            for (int k = 0; k < n; k++) rset_clr(&live, tmpArr[k]);
            instr_uses(in, nextVreg, tmpArr, &n);
            for (int k = 0; k < n; k++) rset_set(&live, tmpArr[k]);
            implicit_uses(in, nextVreg, tmpArr, &n);
            for (int k = 0; k < n; k++) rset_set(&live, tmpArr[k]);
        }
        rset_free(&live);
    }

    rset_free(&tmp);
    for (int b = 0; b < nBlocks; b++) { rset_free(&use[b]); rset_free(&def[b]); rset_free(&liveIn[b]); rset_free(&liveOut[b]); }
    free(use); free(def); free(liveIn); free(liveOut);
    return r;
}
static void liveness_free(MLiveness *r, int count) {
    for (int i = 0; i < count; i++) rset_free(&r->liveAfter[i]);
    free(r->liveAfter);
}

/* =========================================================================
 * Fase 2: Interference Graph — matrice bit triangolare + adjlist.
 * ========================================================================= */
typedef struct { int *data, len, cap; } AdjList;
static void adj_append(AdjList *l, int v) {
    if (l->len == l->cap) { l->cap = l->cap ? l->cap * 2 : 4; l->data = realloc(l->data, (size_t)l->cap * sizeof(int)); }
    l->data[l->len++] = v;
}

typedef struct {
    int n;
    uint64_t *matrix;
    AdjList  *adj;
    int      *degree;
    int      *color;
    int      *active;
    uint32_t *excl;
    int      *spillCost;
    char     *crossesCall;
} IGraph;

static long tri_idx(int i, int j) {
    if (i < j) { int t = i; i = j; j = t; }
    return (long)i * (i - 1) / 2 + j;
}
static int mat_test(const uint64_t *m, int i, int j) {
    long idx = tri_idx(i, j); return (int)((m[idx >> 6] >> (idx & 63)) & 1ULL);
}
static void mat_set(uint64_t *m, int i, int j) {
    long idx = tri_idx(i, j); m[idx >> 6] |= 1ULL << (idx & 63);
}
static void ig_add_edge(IGraph *g, int i, int j) {
    if (i == j || i < 0 || j < 0) return;
    if (mat_test(g->matrix, i, j)) return;
    mat_set(g->matrix, i, j);
    adj_append(&g->adj[i], j);
    adj_append(&g->adj[j], i);
    g->degree[i]++;
    g->degree[j]++;
}

/* Costo di spill pesato per annidamento loop (EaC 13.4.2): 10^depth,
 * cap a depth 5 per restare in range int con conteggi ragionevoli.
 * loopDepth arriva stampigliato dall'IR (ir.c), propagato attraverso
 * l'instruction selector (g_curLoopDepth in instr_selector.c) fino a
 * MachInstr.loopDepth. */
static const int LOOP_WEIGHT[] = { 1, 10, 100, 1000, 10000, 100000 };
#define LOOP_WEIGHT_MAX_IDX 5
static int spill_weight(int loopDepth) {
    if (loopDepth < 0) loopDepth = 0;
    if (loopDepth > LOOP_WEIGHT_MAX_IDX) loopDepth = LOOP_WEIGHT_MAX_IDX;
    return LOOP_WEIGHT[loopDepth];
}

static IGraph build_graph(const MachFunction *f, RBlock *blocks, int nBlocks,
                          int nextVreg, MLiveness *liv) {
    IGraph g;
    g.n = nextVreg + PHYS_ALLOCATABLE;
    long nbits = (long)g.n * (g.n - 1) / 2;
    g.matrix = calloc((size_t)((nbits + 63) / 64 + 1), sizeof(uint64_t));
    g.adj = calloc((size_t)g.n, sizeof(AdjList));
    g.degree = calloc((size_t)g.n, sizeof(int));
    g.color = malloc((size_t)g.n * sizeof(int));
    g.active = malloc((size_t)g.n * sizeof(int));
    g.excl = calloc((size_t)g.n, sizeof(uint32_t));
    g.spillCost = calloc((size_t)g.n, sizeof(int));
    g.crossesCall = calloc((size_t)g.n, 1);

    for (int i = 0; i < g.n; i++) { g.color[i] = -1; g.active[i] = 1; }
    for (int p = 0; p < PHYS_ALLOCATABLE; p++) g.color[nextVreg + p] = p;

    int tmpArr[16];
    for (int b = 0; b < nBlocks; b++) {
        for (int i = blocks[b].start; i < blocks[b].end; i++) {
            const MachInstr *in = &f->instrs[i];
            int n, w = spill_weight(in->loopDepth);

            instr_uses(in, nextVreg, tmpArr, &n);
            for (int k = 0; k < n; k++) if (tmpArr[k] < nextVreg) g.spillCost[tmpArr[k]] += w;
            instr_defs(in, nextVreg, tmpArr, &n);
            for (int k = 0; k < n; k++) if (tmpArr[k] < nextVreg) g.spillCost[tmpArr[k]] += w;

            int defs[8], nd, idefs[16], nid;
            instr_defs(in, nextVreg, defs, &nd);
            implicit_defs(in, nextVreg, idefs, &nid);

            for (int d = 0; d < nd; d++) {
                RSET_FOREACH(&liv->liveAfter[i], id)
                    ig_add_edge(&g, defs[d], id);
                RSET_FOREACH_END
            }
            for (int d = 0; d < nid; d++) {
                RSET_FOREACH(&liv->liveAfter[i], id)
                    ig_add_edge(&g, idefs[d], id);
                RSET_FOREACH_END
            }

            if (in->op == MACH_CALL) {
                RSET_FOREACH(&liv->liveAfter[i], id)
                    if (id < nextVreg) {
                        g.excl[id] |= ((1U << PHYS_CALLER_SAVED_COUNT) - 1);
                        g.crossesCall[id] = 1;
                    }
                RSET_FOREACH_END
            } else if (in->op == MACH_IDIV || in->op == MACH_CQO) {
                uint32_t mask = (1U << PHYS_RAX) | (1U << PHYS_RDX);
                RSET_FOREACH(&liv->liveAfter[i], id)
                    if (id < nextVreg) g.excl[id] |= mask;
                RSET_FOREACH_END
            } else if (is_setcc(in->op)) {
                uint32_t mask = (1U << PHYS_RAX);
                RSET_FOREACH(&liv->liveAfter[i], id)
                    if (id < nextVreg) g.excl[id] |= mask;
                RSET_FOREACH_END
            }
        }
    }
    return g;
}
static void graph_free(IGraph *g) {
    for (int i = 0; i < g->n; i++) free(g->adj[i].data);
    free(g->adj); free(g->matrix); free(g->degree); free(g->color);
    free(g->active); free(g->excl); free(g->spillCost); free(g->crossesCall);
}

/* =========================================================================
 * Fase 3: Simplify (Briggs-optimistic) con bucket-by-degree, O(1) amm.
 * ========================================================================= */
typedef struct {
    int *head, *bnext, *bprev, *inBucket;
    int  k;
} Buckets;

static Buckets buckets_create(int nextVreg, int k) {
    Buckets b; b.k = k;
    b.head = malloc((size_t)k * sizeof(int));
    for (int d = 0; d < k; d++) b.head[d] = -1;
    b.bnext = malloc((size_t)(nextVreg ? nextVreg : 1) * sizeof(int));
    b.bprev = malloc((size_t)(nextVreg ? nextVreg : 1) * sizeof(int));
    b.inBucket = calloc((size_t)(nextVreg ? nextVreg : 1), sizeof(int));
    return b;
}
static void buckets_free(Buckets *b) { free(b->head); free(b->bnext); free(b->bprev); free(b->inBucket); }
static void bucket_insert(Buckets *b, int v, int d) {
    b->bprev[v] = -1;
    b->bnext[v] = b->head[d];
    if (b->head[d] >= 0) b->bprev[b->head[d]] = v;
    b->head[d] = v;
    b->inBucket[v] = 1;
}
static void bucket_remove(Buckets *b, int v, int d) {
    if (b->bprev[v] >= 0) b->bnext[b->bprev[v]] = b->bnext[v];
    else b->head[d] = b->bnext[v];
    if (b->bnext[v] >= 0) b->bprev[b->bnext[v]] = b->bprev[v];
    b->inBucket[v] = 0;
}
static int bucket_pop_any_low(Buckets *b, int *outD) {
    for (int d = 0; d < b->k; d++)
        if (b->head[d] >= 0) { *outD = d; return b->head[d]; }
    return -1;
}

static int *simplify(IGraph *g, int nextVreg, int *outStackLen) {
    int *stack = malloc((size_t)(nextVreg ? nextVreg : 1) * sizeof(int));
    int stackLen = 0;
    int k = PHYS_ALLOCATABLE;
    Buckets buckets = buckets_create(nextVreg, k);

    int remaining = nextVreg;
    for (int v = 0; v < nextVreg; v++)
        if (g->degree[v] < k) bucket_insert(&buckets, v, g->degree[v]);

    while (remaining > 0) {
        int d, chosen = bucket_pop_any_low(&buckets, &d);

        if (chosen >= 0) {
            bucket_remove(&buckets, chosen, d);
        } else {
            /* nessun nodo trivialmente colorabile: vittima tra grado>=k,
             * fuori dai bucket. Fallback O(n), path raro (solo su blocco). */
            double best = 1e18;
            for (int v = 0; v < nextVreg; v++) {
                if (!g->active[v] || buckets.inBucket[v]) continue;
                double ratio = g->degree[v] > 0 ? (double)g->spillCost[v] / g->degree[v] : 0.0;
                if (ratio < best) { best = ratio; chosen = v; }
            }
        }
        if (chosen < 0) break;   /* difensivo */

        g->active[chosen] = 0;
        remaining--;
        stack[stackLen++] = chosen;

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
    *outStackLen = stackLen;
    return stack;
}

/* =========================================================================
 * Fase 4: Select.
 * ========================================================================= */
static int select_colors(IGraph *g, int nextVreg, int *stack, int stackLen, int *spilled, int *outNSpilled) {
    (void)nextVreg;
    int nSpilled = 0;
    for (int si = stackLen - 1; si >= 0; si--) {
        int v = stack[si];
        g->active[v] = 1;

        uint32_t forbidden = g->excl[v];
        for (int k = 0; k < g->adj[v].len; k++) {
            int w = g->adj[v].data[k];
            if (g->color[w] >= 0) forbidden |= (1U << g->color[w]);
        }

        int chosen = -1;
        if (g->crossesCall[v]) {
            for (int p = PHYS_CALLER_SAVED_COUNT; p < PHYS_ALLOCATABLE; p++)
                if (!((forbidden >> p) & 1U)) { chosen = p; break; }
            if (chosen < 0)
                for (int p = 0; p < PHYS_CALLER_SAVED_COUNT; p++)
                    if (!((forbidden >> p) & 1U)) { chosen = p; break; }
        } else {
            for (int p = 0; p < PHYS_ALLOCATABLE; p++)
                if (!((forbidden >> p) & 1U)) { chosen = p; break; }
        }

        if (chosen >= 0) g->color[v] = chosen;
        else { g->color[v] = -2; spilled[nSpilled++] = v; }
    }
    *outNSpilled = nSpilled;
    return nSpilled == 0;
}

/* =========================================================================
 * Fase 5: Spill code insertion, con cache di reload (reuse temp per letture
 * consecutive dello stesso slot entro un blocco base). La cache si
 * invalida su MACH_LABEL (inizio nuovo blocco: valore non garantito) e
 * dopo ogni salto/call/ret (fine blocco/possibile clobber). Una scrittura
 * ad un vreg spillato aggiorna la cache con il tmp appena calcolato,
 * cosi' letture successive nello stesso blocco riusano quel tmp senza
 * un reload dallo stack (EaC 13.4.4: store-once, read-many locale).
 * ========================================================================= */
static int is_spilled(const int *spilled, int nSpilled, int v) {
    for (int i = 0; i < nSpilled; i++) if (spilled[i] == v) return 1;
    return 0;
}

static void invalidate_cache(int *cache, int n) {
    for (int i = 0; i < n; i++) cache[i] = -1;
}

static void load_spilled(MachOperand *o, int origVreg, MachFunction *f, int off,
                          MachInstr *newInstrs, int *newCount, int *cache) {
    if (cache[origVreg] >= 0) {
        o->kind = MO_VREG; o->vregId = cache[origVreg];
        return;
    }
    int tmp = f->nextVreg++;
    MachInstr ld; memset(&ld, 0, sizeof ld);
    ld.op = MACH_MOV; ld.dst.kind = MO_VREG; ld.dst.vregId = tmp;
    ld.src1.kind = MO_STACK; ld.src1.stackOff = off;
    ld.src2.kind = MO_NONE; ld.scale = 8;
    newInstrs[(*newCount)++] = ld;
    o->kind = MO_VREG; o->vregId = tmp;
    cache[origVreg] = tmp;
}

static void spill_insert(MachFunction *f, const int *spilled, int nSpilled, int *frameOff) {
    int origNextVreg = f->nextVreg;   /* snapshot: cache indicizzata solo sui vreg originali */

    int *slot = malloc((size_t)origNextVreg * sizeof(int));
    for (int i = 0; i < origNextVreg; i++) slot[i] = -1;
    for (int i = 0; i < nSpilled; i++) {
        *frameOff += 8;
        slot[spilled[i]] = *frameOff;
    }

    int *cache = malloc((size_t)origNextVreg * sizeof(int));
    invalidate_cache(cache, origNextVreg);

    int maxNew = f->count * 3 + 16;
    MachInstr *newInstrs = malloc((size_t)maxNew * sizeof(MachInstr));
    int newCount = 0;

    for (int i = 0; i < f->count; i++) {
        MachInstr in = f->instrs[i];
        int isStore = (in.op == MACH_STORE);

        if (in.op == MACH_LABEL) invalidate_cache(cache, origNextVreg);

        MachOperand *srcs[2] = { &in.src1, &in.src2 };
        for (int s = 0; s < 2; s++) {
            if (srcs[s]->kind == MO_VREG && srcs[s]->vregId < origNextVreg &&
                is_spilled(spilled, nSpilled, srcs[s]->vregId)) {
                int orig = srcs[s]->vregId;
                load_spilled(srcs[s], orig, f, slot[orig], newInstrs, &newCount, cache);
            }
        }

        MachOperand *memHolder = isStore ? &in.dst : &in.src1;
        if (memHolder->kind == MO_MEM) {
            if (memHolder->mem.baseVreg >= 0 && memHolder->mem.baseVreg < origNextVreg &&
                is_spilled(spilled, nSpilled, memHolder->mem.baseVreg)) {
                int orig = memHolder->mem.baseVreg;
                MachOperand tmpOp; tmpOp.kind = MO_VREG; tmpOp.vregId = orig;
                load_spilled(&tmpOp, orig, f, slot[orig], newInstrs, &newCount, cache);
                memHolder->mem.baseVreg = tmpOp.vregId;
            }
            if (memHolder->mem.indexVreg >= 0 && memHolder->mem.indexVreg < origNextVreg &&
                is_spilled(spilled, nSpilled, memHolder->mem.indexVreg)) {
                int orig = memHolder->mem.indexVreg;
                MachOperand tmpOp; tmpOp.kind = MO_VREG; tmpOp.vregId = orig;
                load_spilled(&tmpOp, orig, f, slot[orig], newInstrs, &newCount, cache);
                memHolder->mem.indexVreg = tmpOp.vregId;
            }
        }

        int dstSpilled = (in.dst.kind == MO_VREG) && !isStore &&
                          in.dst.vregId < origNextVreg &&
                          is_spilled(spilled, nSpilled, in.dst.vregId);
        int dstStoreOff = -1, origDstVreg = -1;
        if (dstSpilled) {
            origDstVreg = in.dst.vregId;
            dstStoreOff = slot[origDstVreg];   /* salvato PRIMA di riassegnare in.dst */
            int dstTmp;
            if (is_rmw(in.op)) {
                /* RMW: dst e' anche sorgente, serve il valore corrente */
                MachOperand tmpOp; tmpOp.kind = MO_VREG; tmpOp.vregId = origDstVreg;
                load_spilled(&tmpOp, origDstVreg, f, dstStoreOff, newInstrs, &newCount, cache);
                dstTmp = tmpOp.vregId;
            } else {
                dstTmp = f->nextVreg++;
            }
            in.dst.vregId = dstTmp;
        }

        newInstrs[newCount++] = in;

        if (dstSpilled) {
            MachInstr st; memset(&st, 0, sizeof st);
            st.op = MACH_MOV;
            st.dst.kind = MO_STACK; st.dst.stackOff = dstStoreOff;
            st.src1.kind = MO_VREG; st.src1.vregId = in.dst.vregId;
            st.src2.kind = MO_NONE; st.scale = 8;
            newInstrs[newCount++] = st;
            cache[origDstVreg] = in.dst.vregId;   /* valore appena scritto: cache-alo */
        }

        if (is_ctrl_transfer(in.op)) invalidate_cache(cache, origNextVreg);
    }

    free(f->instrs);
    f->instrs = newInstrs;
    f->count = newCount;
    f->capacity = maxNew;
    free(slot);
    free(cache);
}

/* =========================================================================
 * Fase 6: Rewrite finale MO_VREG -> MO_PHYS.
 * ========================================================================= */
static void rewrite_phys(MachOperand *o, const int *color) {
    if (o->kind == MO_VREG) { o->kind = MO_PHYS; o->physReg = color[o->vregId]; }
}
static void finalize_colors(MachFunction *f, const int *color) {
    for (int i = 0; i < f->count; i++) {
        MachInstr *in = &f->instrs[i];
        rewrite_phys(&in->dst, color);
        rewrite_phys(&in->src1, color);
        rewrite_phys(&in->src2, color);
        if (in->dst.kind == MO_MEM) {
            if (in->dst.mem.baseVreg  >= 0) in->dst.mem.baseVreg  = color[in->dst.mem.baseVreg];
            if (in->dst.mem.indexVreg >= 0) in->dst.mem.indexVreg = color[in->dst.mem.indexVreg];
        }
        if (in->src1.kind == MO_MEM) {
            if (in->src1.mem.baseVreg  >= 0) in->src1.mem.baseVreg  = color[in->src1.mem.baseVreg];
            if (in->src1.mem.indexVreg >= 0) in->src1.mem.indexVreg = color[in->src1.mem.indexVreg];
        }
    }
}
static void remove_identity_moves(MachFunction *f) {
    int newCount = 0;
    for (int i = 0; i < f->count; i++) {
        MachInstr *in = &f->instrs[i];
        if (in->op == MACH_MOV && in->dst.kind == MO_PHYS && in->src1.kind == MO_PHYS &&
            in->dst.physReg == in->src1.physReg) continue;
        f->instrs[newCount++] = *in;
    }
    f->count = newCount;
}

/* =========================================================================
 * Fase 7: Prologo/epilogo callee-saved.
 * ========================================================================= */
static int is_callee_saved(int physReg) { return physReg >= PHYS_RBX && physReg <= PHYS_R15; }

static int count_rets(const MachFunction *f) {
    int n = 0;
    for (int i = 0; i < f->count; i++) if (f->instrs[i].op == MACH_RET) n++;
    return n;
}

static void save_restore_callee(MachFunction *f) {
    uint32_t usedMask = 0;
    for (int i = 0; i < f->count; i++) {
        MachInstr *in = &f->instrs[i];
        if (in->dst.kind == MO_PHYS && is_callee_saved(in->dst.physReg))
            usedMask |= (1U << in->dst.physReg);
    }
    if (usedMask == 0) return;

    int used[PHYS_CALLEE_SAVED_COUNT], nUsed = 0;
    for (int p = PHYS_RBX; p <= PHYS_R15; p++)
        if ((usedMask >> p) & 1U) used[nUsed++] = p;

    int funcBeginIdx = -1;
    for (int i = 0; i < f->count; i++) if (f->instrs[i].op == MACH_FUNC_BEGIN) { funcBeginIdx = i; break; }

    int nRets = count_rets(f);
    MachInstr *newInstrs = malloc((size_t)(f->count + nUsed + nUsed * nRets) * sizeof(MachInstr));
    int nc = 0;
    for (int i = 0; i < f->count; i++) {
        if (i == funcBeginIdx) {
            newInstrs[nc++] = f->instrs[i];
            for (int k = 0; k < nUsed; k++) {
                MachInstr ps; memset(&ps, 0, sizeof ps);
                ps.op = MACH_PUSH; ps.dst.kind = MO_PHYS; ps.dst.physReg = used[k];
                ps.src1.kind = MO_NONE; ps.src2.kind = MO_NONE;
                newInstrs[nc++] = ps;
            }
            continue;
        }
        if (f->instrs[i].op == MACH_RET) {
            for (int k = nUsed - 1; k >= 0; k--) {
                MachInstr po; memset(&po, 0, sizeof po);
                po.op = MACH_POP; po.dst.kind = MO_PHYS; po.dst.physReg = used[k];
                po.src1.kind = MO_NONE; po.src2.kind = MO_NONE;
                newInstrs[nc++] = po;
            }
        }
        newInstrs[nc++] = f->instrs[i];
    }

    free(f->instrs);
    f->instrs = newInstrs;
    f->count = nc;
}

/* =========================================================================
 * Entry point.
 * ========================================================================= */
static int align16(int n) { return (n + 15) & ~15; }

static void regalloc_function(MachFunction *f) {
    int frameOff = 0;

    for (;;) {
        int nBlocks;
        RBlock *blocks = build_cfg(f, &nBlocks);
        MLiveness liv = compute_liveness(f, blocks, nBlocks, f->nextVreg);
        IGraph g = build_graph(f, blocks, nBlocks, f->nextVreg, &liv);

        int stackLen;
        int *stack = simplify(&g, f->nextVreg, &stackLen);

        int *spilled = malloc((size_t)(f->nextVreg ? f->nextVreg : 1) * sizeof(int));
        int nSpilled;
        int ok = select_colors(&g, f->nextVreg, stack, stackLen, spilled, &nSpilled);

        if (ok) {
            finalize_colors(f, g.color);
            free(stack); free(spilled);
            graph_free(&g); liveness_free(&liv, f->count); free(blocks);
            break;
        }

        spill_insert(f, spilled, nSpilled, &frameOff);

        free(stack); free(spilled);
        graph_free(&g); liveness_free(&liv, f->count); free(blocks);
        /* riparte con f->nextVreg aumentato */
    }

    remove_identity_moves(f);
    save_restore_callee(f);
    f->frameSize = align16(frameOff);
}

void regalloc(MachProgram *mp) {
    for (int i = 0; i < mp->count; i++)
        regalloc_function(mp->functions[i]);
}