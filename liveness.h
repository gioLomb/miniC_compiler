#ifndef LIVENESS_H
#define LIVENESS_H

#include <stdint.h>
#include "ir.h"
#include "block.h"
#include "arena.h"
#include "varmap.h"
#include "regalloc_utils.h"

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
void    liveset_clear  (LiveSet *s);
void    liveset_set    (LiveSet *s, int id);
void    liveset_clrbit (LiveSet *s, int id);
int     liveset_test   (const LiveSet *s, int id);
void    liveset_union  (LiveSet *dst, const LiveSet *src);
void    liveset_union_into(LiveSet *dst, const LiveSet *a, const LiveSet *b);
void    liveset_diff   (LiveSet *dst, const LiveSet *a, const LiveSet *b);
int     liveset_equal  (const LiveSet *a, const LiveSet *b);
void    liveset_copy   (LiveSet *dst, const LiveSet *src);

/* Itera solo i bit a 1 (costo proporzionale ai bit settati). */
#define LIVESET_FOREACH(s, idvar) \
    for (int _w = 0; _w < (s)->words; _w++) { \
        uint64_t _bits = (s)->bits[_w]; \
        while (_bits) { \
            int _b = __builtin_ctzll(_bits); \
            int idvar = (_w << 6) + _b; \
            _bits &= (_bits - 1);
#define LIVESET_FOREACH_END } }

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

/*
 * Riceve BasicBlock* (ex RBlock*) al posto del vecchio tipo dedicato.
 */
LivenessResult liveness_compute_mach(const MachFunction *f, const BasicBlock *blocks,
                                      int nBlocks, Arena *arena);

/* ---- Predicati condivisi (fronte IR) ------------------------------------ */

int liveness_defines_dst   (IROp op);
int liveness_is_var_or_temp(OperandKind kind);

#endif