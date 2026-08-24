#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ra_spill.h"
#include "regalloc_utils.h"   /* regalloc_is_rmw, regalloc_is_ctrl_transfer */

/* =========================================================================
 * SpillSet — O(1) bitset for "is this vreg spilled?"
 *
 * Replaces the old O(nSpilled) linear search.
 * ========================================================================= */

typedef struct {
    uint64_t *bits;
    int       cap;   /* number of allocated 64-bit words */
} SpillSet;

static inline void spillset_init(SpillSet *s, int n)
{
    // one bit per vreg id, rounded up to a whole number of 64-bit words
    s->cap  = (n + 63) / 64;
    s->bits = calloc((size_t)s->cap, sizeof(uint64_t));
}

static inline void spillset_free(SpillSet *s) { free(s->bits); }

static inline void spillset_set(SpillSet *s, int v)
{
    // v >> 6 selects the word, v & 63 the bit within it
    s->bits[v >> 6] |= 1ULL << (v & 63);
}

static inline int spillset_test(const SpillSet *s, int v)
{
    return (int)((s->bits[v >> 6] >> (v & 63)) & 1ULL);
}

/* =========================================================================
 * Reload cache — avoids repeated loads of the same slot WITHIN a single
 * original instruction (e.g. an instruction that reads the same spilled
 * value twice, or whose MO_MEM base and index happen to be the same
 * spilled vreg).
 *
 * ---------------------------------------------------------------------
 * IMPORTANT — cache scope and why it is per-instruction, not per-block:
 * ---------------------------------------------------------------------
 * A reload temporary lives from the point it is loaded to the point it is
 * last used. If the cache were allowed to persist across MULTIPLE original
 * instructions (as it previously did — invalidated only on label/jump/call),
 * a spilled value that is read many times within one straight-line block
 * (no branches at all) would produce ONE reload temporary reused across
 * the entire span. That temporary's live range would then be just as long
 * as the original spilled value's — the spill would not have reduced
 * register pressure at all.
 *
 * Concretely: a function with a single basic block (no labels, no jumps)
 * where a parameter is read 20 times to compute 20 values recreates the
 * exact same interference pressure on its cached reload temp as it had on
 * the original vreg. That temp then also fails to color, gets spilled in
 * turn, and its own reload is again cached across the same span — the
 * outer allocation loop in regalloc.c (`for (;;) { ... if (nSpilled==0)
 * break; ra_spill_insert(...); }`) never converges: it hangs.
 *
 * Scoping the cache to a single original instruction bounds every reload
 * temporary's live range to at most the handful of machine instructions
 * that one original instruction expands into. Such a short-lived temp is
 * always trivially colorable (its degree in the interference graph cannot
 * exceed a small constant), so introducing it can itself never require
 * another spill — guaranteeing the allocation loop terminates.
 * ========================================================================= */

static inline void invalidate_cache(int *cache, int n)
{
    // memset with 0xFF gives -1 in two's complement for every int slot,
    // i.e. "no cached reload temporary" for every original vreg
    memset(cache, 0xFF, (size_t)n * sizeof(int));
}

/**
 * Load spilled vreg 'origVreg' from stack slot 'off' into a temporary,
 * reusing the cached temp if already loaded earlier for the SAME original
 * instruction (see cache-scope note above for why the cache is reset before
 * every instruction rather than persisting across the block).
 * Updates *o to reference the (cached or freshly loaded) temporary.
 */
static void load_spilled(MachOperand *o, int origVreg, MachFunction *f,
                         int off, MachInstr *newInstrs, int *newCount,
                         int *cache)
{
    // cache hit: this slot was already reloaded earlier for this same
    // original instruction (e.g. used twice, or as both base and index)
    if (cache[origVreg] >= 0) {
        o->kind   = MO_VREG;
        o->vregId = cache[origVreg];
        return;
    }
    // cache miss: allocate a fresh temp and emit a load from the stack slot
    int tmp = f->nextVreg++;
    MachInstr ld      = {0};
    ld.op             = MACH_MOV;
    ld.dst.kind       = MO_VREG;  ld.dst.vregId   = tmp;
    ld.src1.kind      = MO_STACK; ld.src1.stackOff = off;
    ld.src2.kind      = MO_NONE;
    newInstrs[(*newCount)++] = ld;
    o->kind   = MO_VREG;
    o->vregId = tmp;
    cache[origVreg] = tmp; // remember for subsequent reads within THIS instruction
}

/* =========================================================================
 * ra_spill_insert — rewrites f->instrs with load/store around spills.
 * ========================================================================= */
void ra_spill_insert(MachFunction *f, const int *spilled, int nSpilled,
                     int *frameOff)
{
    int origNextVreg = f->nextVreg;

    // slot[v] = positive stack offset for spilled vreg v; -1 if not spilled
    int *slot = malloc((size_t)origNextVreg * sizeof(int));
    memset(slot, -1, (size_t)origNextVreg * sizeof(int));
    for (int i = 0; i < nSpilled; i++) {
        *frameOff      += 8; // each slot is one 8-byte quadword
        slot[spilled[i]] = *frameOff;
    }

    SpillSet ss;
    spillset_init(&ss, origNextVreg);
    for (int i = 0; i < nSpilled; i++) spillset_set(&ss, spilled[i]);

    int *cache = malloc((size_t)origNextVreg * sizeof(int));
    invalidate_cache(cache, origNextVreg);

    // worst-case overestimate: at most SPILL_MAX_EXPANSION_PER_INSTR new
    // instructions per original one, plus a small fixed safety margin
    int maxNew = f->count * SPILL_MAX_EXPANSION_PER_INSTR + SPILL_EXTRA_MARGIN;
    MachInstr *newInstrs = malloc((size_t)maxNew * sizeof(MachInstr));
    int newCount      = 0;

    for (int i = 0; i < f->count; i++) {
        MachInstr in    = f->instrs[i];
        int isStore     = (in.op == MACH_STORE);

        // Reset the reload cache before every instruction: a cached reload
        // must never be reused across instruction boundaries, or its live
        // range would grow to span the whole block (see module header note
        // above) and defeat the purpose of spilling. This subsumes the old
        // label-only invalidation, since it now happens unconditionally.
        invalidate_cache(cache, origNextVreg);

        /* ---- Reload spilled sources ---- */
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

        /* ---- Reload base/index of MO_MEM operand ---- */
        // STORE reads its address from dst; every other op reads it from src1
        MachOperand *memHolder = isStore ? &in.dst : &in.src1;
        if (memHolder->kind == MO_MEM) {
            if (memHolder->mem.baseVreg >= 0
                && memHolder->mem.baseVreg < origNextVreg
                && spillset_test(&ss, memHolder->mem.baseVreg)) {
                int orig = memHolder->mem.baseVreg;
                MachOperand tmp = { .kind = MO_VREG, .vregId = orig };
                load_spilled(&tmp, orig, f, slot[orig],
                             newInstrs, &newCount, cache);
                memHolder->mem.baseVreg = tmp.vregId;
            }
            if (memHolder->mem.indexVreg >= 0
                && memHolder->mem.indexVreg < origNextVreg
                && spillset_test(&ss, memHolder->mem.indexVreg)) {
                int orig = memHolder->mem.indexVreg;
                MachOperand tmp = { .kind = MO_VREG, .vregId = orig };
                load_spilled(&tmp, orig, f, slot[orig],
                             newInstrs, &newCount, cache);
                memHolder->mem.indexVreg = tmp.vregId;
            }
        }

        /* ---- Spilled destination ---- */
        int dstSpilled  = (in.dst.kind == MO_VREG) && !isStore
                          && in.dst.vregId < origNextVreg
                          && spillset_test(&ss, in.dst.vregId);
        int dstStoreOff = -1;
        int origDstVreg = -1;

        if (dstSpilled) {
            origDstVreg = in.dst.vregId;
            dstStoreOff = slot[origDstVreg];
            int dstTmp;
            if (regalloc_is_rmw(in.op)) {
                // read-modify-write: the current value must be loaded first,
                // since the instruction reads dst before writing it
                MachOperand tmp = { .kind = MO_VREG, .vregId = origDstVreg };
                load_spilled(&tmp, origDstVreg, f, dstStoreOff,
                             newInstrs, &newCount, cache);
                dstTmp = tmp.vregId;
            } else {
                // pure write: no need to reload the old value, just a fresh temp
                dstTmp = f->nextVreg++;
            }
            in.dst.vregId = dstTmp;
        }

        newInstrs[newCount++] = in;

        // store the result back into its slot
        if (dstSpilled) {
            MachInstr st   = {0};
            st.op          = MACH_MOV;
            st.dst.kind    = MO_STACK; st.dst.stackOff  = dstStoreOff;
            st.src1.kind   = MO_VREG;  st.src1.vregId   = in.dst.vregId;
            st.src2.kind   = MO_NONE;
            newInstrs[newCount++] = st;
            // the value just stored is now also the freshest reload for this
            // slot, but ONLY for the remainder of this same instruction's
            // expansion (the cache is reset again at the top of the next
            // loop iteration, see above) — kept for symmetry/documentation,
            // harmless since nothing after this point reads the cache again
            // within this iteration.
            cache[origDstVreg]   = in.dst.vregId;
        }
    }

    free(f->instrs);
    f->instrs   = newInstrs;
    f->count    = newCount;
    f->capacity = maxNew;
    free(slot);
    free(cache);
    spillset_free(&ss);
}