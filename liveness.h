#ifndef LIVENESS_H
#define LIVENESS_H

#include <stdint.h>
#include "ir.h"
#include "arena.h"
#include "varmap.h"           /* VarMap */
#include "regalloc_utils.h"   /* MachFunction, RBlock, instr_uses&co: fronte Mach */

/* =========================================================================
 * Modulo liveness condiviso da DCE/LICM/SR (IR lineare) e da regalloc/
 * interference (codice macchina). Stesso motore di dataflow in entrambi
 * i casi (liveness_compute_core); l'unica parte specifica per fronte e'
 * l'estrazione uses/defs da un'istruzione (irExtract/machExtract, statiche
 * in liveness.c), incapsulata dietro le due funzioni pubbliche
 * liveness_compute_ir()/liveness_compute_mach(). Nessun'altra funzione di
 * "compute" generica e' esposta: chi vuole la liveness sceglie il fronte
 * giusto, non compone il motore a mano.
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

/* Blocco base per il dataflow (indipendente da IRBlock/RBlock: entrambi
   i fronti vi si mappano prima di chiamare il motore). */
typedef struct {
    int start, end;
    int succ[2];
} LivenessBlock;

/* Estrae uses/defs (id gia' mappati in [0,numVars)) dell'istruzione 'instrIdx'. */
typedef void (*LivenessExtractFn)(void *ctx, int instrIdx,
                                   int uses[LIVENESS_MAX_IDS], int *nUses,
                                   int defs[LIVENESS_MAX_IDS], int *nDefs);

/*
 * Use/Def per blocco + risultato del dataflow backward (LiveIn/LiveOut).
 * Annidato dentro LivenessResult, non da costruire a mano dal chiamante.
 */
typedef struct {
    LiveSet *Use, *Def, *LiveIn, *LiveOut;
    int numVars, words;
} LivenessBlockSets;

/*
 * Motore dataflow backward a punto fisso.
 * Tutti i bits dei quattro array LiveSet sono allocati in un unico blocco
 * contiguo nell'arena e azzerati con un solo memset.
 */
LivenessBlockSets liveness_compute_core(int nBlocks, const LivenessBlock *blocks,
                                         int numVars, const char *reachable,
                                         LivenessExtractFn extract, void *ctx,
                                         Arena *arena);

/* liveAfter[i] = vivi subito dopo l'istruzione i. Richiede LiveOut gia' calcolato. */
LiveSet *liveness_compute_per_instr(int nBlocks, const LivenessBlock *blocks,
                                     int instrCount, int numVars,
                                     const LiveSet *blockLiveOut,
                                     LivenessExtractFn extract, void *ctx,
                                     Arena *arena);

/* ---- Risultato esposto ai due fronti ------------------------------------ */

typedef struct {
    LivenessBlockSets blockSets;   /* Use/Def/LiveIn/LiveOut a livello di blocco */
    LiveSet          *liveAfter;   /* per-istruzione; NULL nel fronte IR (non richiesta) */
    VarMap            varMap;      /* Operand IR -> id; non significativa nel fronte Mach */
} LivenessResult;

/* ---- Fronte IR lineare (DCE/LICM/SR) ------------------------------------ */

/*
 * Calcola Use/Def/LiveIn/LiveOut per tutti i blocchi di 'f'. Costruisce
 * internamente la VarMap (Operand IR -> id) con una prescan di f->instrs
 * (fissa numVars prima di allocare i bitset). Memoria in 'arena'.
 * 'reachable' opzionale (NULL = tutti raggiungibili). liveAfter NON
 * calcolata (resta NULL): DCE/LICM/SR non ne hanno bisogno.
 */
LivenessResult liveness_compute_ir(IRFunction *f, const char *reachable, Arena *arena);

/* ---- Fronte codice macchina (regalloc/interference) --------------------- */

/*
 * Calcola Use/Def/LiveIn/LiveOut + liveAfter per tutte le istruzioni di
 * 'f' (codice macchina gia' selezionato/schedulato). 'blocks'/'nBlocks'
 * e' il CFG macchina (RBlock, costruito dal chiamante con build_cfg in
 * regalloc.c). Risorse tracciate: vreg [0, f->nextVreg) piu' i registri
 * fisici allocabili offsettati da f->nextVreg (PHYS_ALLOCATABLE),
 * inclusi usi/def IMPLICITI (RAX/RDX per IDIV, caller-saved per CALL).
 */
LivenessResult liveness_compute_mach(const MachFunction *f, const RBlock *blocks,
                                      int nBlocks, Arena *arena);

/* ---- Predicati condivisi (fronte IR) ------------------------------------ */

int liveness_defines_dst   (IROp op);
int liveness_is_var_or_temp(OperandKind kind);

#endif