#ifndef LIVENESS_H
#define LIVENESS_H

#include <stdint.h>
#include "ir.h"
#include "block.h"
#include "arena.h"
#include "varmap.h"
#include "regalloc_utils.h"

#define BITS_PER_WORD 64
/* =========================================================================
 * Modulo liveness condiviso da DCE/LICM/SR (IR lineare) e da regalloc/
 * interference (codice macchina). Stesso motore di dataflow in entrambi
 * i casi (liveness_compute_core); l'unica parte specifica per fronte e'
 * l'estrazione uses/defs da un'istruzione (irExtract/machExtract, statiche
 * in liveness.c), incapsulata dietro le due funzioni pubbliche
 * liveness_compute_ir()/liveness_compute_mach().
 *
 * LivenessBlock rimosso: BasicBlock (block.h) usato direttamente come
 * tipo blocco in tutte le firme di questo modulo.
 * ========================================================================= */

/* ---- LiveSet: unico bitset del progetto --------------------------------- */

typedef struct {
    uint64_t *bits;
    int       words;
} LiveSet;

LiveSet liveset_new    (Arena *arena, int words);
void    liveset_set    (LiveSet *s, int id);
void    liveset_clrbit (LiveSet *s, int id);
int     liveset_test   (const LiveSet *s, int id);
int     liveset_equal  (const LiveSet *a, const LiveSet *b);
void    liveset_copy   (LiveSet *dst, const LiveSet *src);

/* ---- LiveSetIter: iteratore sui bit settati ----------------------------
 *
 * Sostituisce le vecchie macro LIVESET_FOREACH / LIVESET_FOREACH_END.
 *
 * Uso:
 *   int id;
 *   for (LiveSetIter it = LIVESET_ITER(s); LIVESET_NEXT(&it, &id); )
 *       ... usa id ...
 *
 * Vantaggi rispetto alle vecchie macro a due parti:
 *   - parentesi sempre bilanciate, nessun END da ricordare
 *   - debugger e breakpoint funzionano normalmente
 *   - nessuna collisione di nomi interni con variabili del call-site
 *   - garantito inline (macro), zero dipendenza dall'ottimizzatore
 * ----------------------------------------------------------------------- */

typedef struct {
    const LiveSet *s;
    int            word;
    uint64_t       bits;
} LiveSetIter;

/* Inizializza l'iteratore sul LiveSet 's'. */
#define LIVESET_ITER(s) \
    { (s), 0, ((s)->words > 0 ? (s)->bits[0] : 0ULL) }

/*
 * Avanza l'iteratore e scrive il prossimo id in *out_id.
 * Restituisce 1 se trovato, 0 se esaurito.
 * 's' in LIVESET_ITER e 'it'/'out_id' in LIVESET_NEXT sono valutati
 * una sola volta: nessun rischio di doppia valutazione.
 */
#define LIVESET_NEXT(it, out_id)                                    \
    (liveset_iter_next_impl((it), (out_id)))

/* Implementazione interna — non usare direttamente, usa LIVESET_NEXT. */
static inline int liveset_iter_next_impl(LiveSetIter *it, int *out_id) {
    while (it->bits == 0) {
        it->word++;
        if (it->word >= it->s->words) return 0;
        it->bits = it->s->bits[it->word];
    }
    int b    = __builtin_ctzll(it->bits);
    *out_id  = (it->word << 6) + b;
    it->bits &= it->bits - 1;   /* clear lowest set bit */
    return 1;
}

/* ---- Motore di dataflow generico ---------------------------------------- */

/* Massimo usi/defs per istruzione (caso peggiore: MACH_CALL ha 9 caller-saved) */
#define LIVENESS_MAX_IDS 16

/* Estrae uses/defs (id gia' mappati in [0,numVars)) dell'istruzione 'instrIdx'. */
typedef void (*LivenessExtractFn)(void *ctx, int instrIdx,
                                   int uses[LIVENESS_MAX_IDS], int *nUses,
                                   int defs[LIVENESS_MAX_IDS], int *nDefs);

/*
 * Use/Def per blocco + risultato del dataflow backward (LiveIn/LiveOut).
 */
typedef struct {
    LiveSet *Use, *Def, *LiveIn, *LiveOut;
    int numVars, words;
} LivenessBlockSets;

/*
 * Motore dataflow backward a punto fisso.
 * Riceve BasicBlock* al posto del vecchio LivenessBlock*.
 */
LivenessBlockSets liveness_compute_core(int nBlocks, const BasicBlock *blocks,
                                         int numVars, const char *reachable,
                                         LivenessExtractFn extract, void *ctx,
                                         Arena *arena);

/* liveAfter[i] = vivi subito dopo l'istruzione i. Richiede LiveOut gia' calcolato. */
LiveSet *liveness_compute_per_instr(int nBlocks, const BasicBlock *blocks,
                                     int instrCount, int numVars,
                                     const LiveSet *blockLiveOut,
                                     LivenessExtractFn extract, void *ctx,
                                     Arena *arena);

/* ---- Risultato esposto ai due fronti ------------------------------------ */

typedef struct {
    LivenessBlockSets blockSets;   /* Use/Def/LiveIn/LiveOut a livello di blocco */
    LiveSet          *liveAfter;   /* per-istruzione; NULL nel fronte IR         */
    VarMap            varMap;      /* Operand IR -> id; non usata nel fronte Mach */
} LivenessResult;

/* ---- Fronte IR lineare (DCE/LICM/SR) ------------------------------------ */

LivenessResult liveness_compute_ir(IRFunction *f, const char *reachable, Arena *arena);

/* ---- Fronte codice macchina (regalloc/interference) --------------------- */

LivenessResult liveness_compute_mach(const MachFunction *f, const BasicBlock *blocks,
                                      int nBlocks, Arena *arena);

/* ---- Predicati condivisi (fronte IR) ------------------------------------ */

int liveness_defines_dst   (IROp op);
int liveness_is_var_or_temp(OperandKind kind);

#endif