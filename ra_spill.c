#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ra_spill.h"
#include "regalloc_utils.h"   /* regalloc_is_rmw, regalloc_is_ctrl_transfer */

/* =========================================================================
 * SpillSet — bitset O(1) per "è questo vreg spillato?"
 *
 * Rimpiazza la vecchia ricerca lineare O(nSpilled).
 * ========================================================================= */

typedef struct {
    uint64_t *bits;
    int       cap;   /* parole da 64 bit allocate */
} SpillSet;

static inline void spillset_init(SpillSet *s, int n)
{
    s->cap  = (n + 63) / 64;
    s->bits = calloc((size_t)s->cap, sizeof(uint64_t));
}

static inline void spillset_free(SpillSet *s) { free(s->bits); }

static inline void spillset_set(SpillSet *s, int v)
{
    s->bits[v >> 6] |= 1ULL << (v & 63);
}

static inline int spillset_test(const SpillSet *s, int v)
{
    return (int)((s->bits[v >> 6] >> (v & 63)) & 1ULL);
}

/* =========================================================================
 * Cache reload — evita load ripetuti dello stesso slot dentro un BB.
 * Invalidata a ogni label/salto/call (potenziali cambi di flusso).
 * ========================================================================= */

static inline void invalidate_cache(int *cache, int n)
{
    /* memset con 0xFF: -1 in complemento a due per ogni int */
    memset(cache, 0xFF, (size_t)n * sizeof(int));
}

/* Carica il vreg spillato 'origVreg' dallo slot 'off' in un temporaneo,
   riusando quello in cache se disponibile. Aggiorna *o al temporaneo. */
static void load_spilled(MachOperand *o, int origVreg, MachFunction *f,
                         int off, MachInstr *newInstrs, int *newCount,
                         int *cache)
{
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

/* =========================================================================
 * ra_spill_insert — riscrive f->instrs con load/store attorno agli spill.
 * ========================================================================= */
void ra_spill_insert(MachFunction *f, const int *spilled, int nSpilled,
                     int *frameOff)
{
    int origNextVreg = f->nextVreg;

    /* slot[v] = offset stack positivo per il vreg v spillato; -1 se non spillato */
    int *slot = malloc((size_t)origNextVreg * sizeof(int));
    memset(slot, -1, (size_t)origNextVreg * sizeof(int));
    for (int i = 0; i < nSpilled; i++) {
        *frameOff      += 8;
        slot[spilled[i]] = *frameOff;
    }

    SpillSet ss;
    spillset_init(&ss, origNextVreg);
    for (int i = 0; i < nSpilled; i++) spillset_set(&ss, spilled[i]);

    int *cache = malloc((size_t)origNextVreg * sizeof(int));
    invalidate_cache(cache, origNextVreg);

    /* Sovrastima: al massimo 3 istruzioni per istruzione originale + margine */
    int maxNew        = f->count * 3 + 16;
    MachInstr *newInstrs = malloc((size_t)maxNew * sizeof(MachInstr));
    int newCount      = 0;

    for (int i = 0; i < f->count; i++) {
        MachInstr in    = f->instrs[i];
        int isStore     = (in.op == MACH_STORE);

        /* label: invalida cache (inizio nuovo BB, flusso sconosciuto) */
        if (in.op == MACH_LABEL) invalidate_cache(cache, origNextVreg);

        /* ---- Ricarica sorgenti spillate ---- */
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

        /* ---- Ricarica base/index di MO_MEM ---- */
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

        /* ---- Destinazione spillata ---- */
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
                /* read-modify-write: serve caricare il valore corrente prima */
                MachOperand tmp = { .kind = MO_VREG, .vregId = origDstVreg };
                load_spilled(&tmp, origDstVreg, f, dstStoreOff,
                             newInstrs, &newCount, cache);
                dstTmp = tmp.vregId;
            } else {
                dstTmp = f->nextVreg++;
            }
            in.dst.vregId = dstTmp;
        }

        newInstrs[newCount++] = in;

        /* store del risultato nel suo slot */
        if (dstSpilled) {
            MachInstr st   = {0};
            st.op          = MACH_MOV;
            st.dst.kind    = MO_STACK; st.dst.stackOff  = dstStoreOff;
            st.src1.kind   = MO_VREG;  st.src1.vregId   = in.dst.vregId;
            st.src2.kind   = MO_NONE;
            newInstrs[newCount++] = st;
            cache[origDstVreg]   = in.dst.vregId;
        }

        /* salti/call: invalida cache (prossima istruzione può essere raggiunta
           da altri BB con stato cache diverso) */
        if (regalloc_is_ctrl_transfer(in.op))
            invalidate_cache(cache, origNextVreg);
    }

    free(f->instrs);
    f->instrs   = newInstrs;
    f->count    = newCount;
    f->capacity = maxNew;
    free(slot);
    free(cache);
    spillset_free(&ss);
}
