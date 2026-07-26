#ifndef LIVENESS_H
#define LIVENESS_H

#include <stdint.h>
#include "ir.h"
#include "arena.h"
#include "hash_table.h"

/* =========================================================================
 * Modulo di liveness analysis condiviso tra DCE e LICM.
 *
 * Espone tre famiglie di primitive:
 *
 *   VarMap  — mappa (tipo, level/tempId, offset) → ID intero compatto,
 *             usata per indicizzare i bitset. Stessa chiave uint64_t
 *             usata in dce.c e cp.c, ora condivisa.
 *
 *   LiveSet — bitset di variabili vive, con le operazioni elementari
 *             (set, clear, union, difference, equal, copy).
 *             Allocato dentro un'Arena: nessuna free individuale.
 *
 *   liveness_compute — calcola Use/Def/LiveIn/LiveOut per tutti i
 *             blocchi di una funzione in un'unica chiamata. I risultati
 *             vivono nell'arena passata dal chiamante: il chiamante
 *             decide quando liberarli (arena_destroy).
 * ========================================================================= */

/* ---- VarMap ------------------------------------------------------------ */

typedef struct {
    Hash_Table *table;
    int         nextId;
} VarMap;

unsigned long varmap_hash(const void *key, size_t keySize);
uint64_t      varmap_make_key(int kind, int a, int b);
int           varmap_id(VarMap *m, int kind, int a, int b);
int           varmap_operand_id(VarMap *m, Operand op);
void          varmap_init(VarMap *m);
void          varmap_destroy(VarMap *m);

/* ---- LiveSet ----------------------------------------------------------- */

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

/* ---- Liveness analysis ------------------------------------------------- */

typedef struct {
    LiveSet *Use;
    LiveSet *Def;
    LiveSet *LiveIn;
    LiveSet *LiveOut;
    VarMap   varMap;
    int      numVars;
    int      words;
} LivenessResult;

/*
 * Calcola Use, Def, LiveIn, LiveOut per tutti i blocchi di 'f'.
 * Tutta la memoria vive nell'arena passata: il chiamante libera con
 * arena_destroy quando non serve piu'. La hash table di varMap viene
 * pero' liberata separatamente con varmap_destroy(&result.varMap)
 * perche' usa malloc interno non gestito dall'arena.
 *
 * 'reachable' e' un array char[f->blockCount] opzionale: se non NULL,
 * i blocchi con reachable[b]==0 vengono ignorati (Use/Def restano zero).
 * Passare NULL equivale a considerare tutti i blocchi raggiungibili.
 */
LivenessResult liveness_compute(IRFunction *f, const char *reachable, Arena *arena);

/* ---- Predicati condivisi ----------------------------------------------- */

int liveness_defines_dst   (IROp op);
int liveness_is_var_or_temp(OperandKind kind);

#endif
