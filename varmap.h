#ifndef VARMAP_H
#define VARMAP_H

#include <stdint.h>
#include "ir.h"         /* Operand, OPND_VAR, OPND_TEMP */
#include "hash_table.h"

/*
 * VarMap: mappa Operand IR (variabile o temporaneo) -> id intero compatto.
 * Usata dal fronte IR di liveness per indicizzare i bitset LiveSet.
 * Separata da liveness.h per consentire l'uso indipendente (es. SVN, SR,
 * LICM costruiscono la propria VarMap senza invocare il dataflow).
 */
typedef struct {
    Hash_Table *table;
    int         nextId;
} VarMap;

/* Hash FNV-1a su chiave uint64_t packed (kind|a|b). */
unsigned long varmap_hash(const void *key, size_t keySize);

/* Costruisce la chiave a 64 bit: 2 bit kind | 31 bit a | 31 bit b. */
uint64_t varmap_make_key(int kind, int a, int b);

/* Restituisce l'id associato a (kind,a,b), creandolo se assente. */
int varmap_id(VarMap *m, int kind, int a, int b);

/*
 * Restituisce l'id dell'Operand se OPND_VAR o OPND_TEMP, -1 altrimenti.
 * Convenzione: kind=0 per VAR (varLevel,varOffset), kind=1 per TEMP (tempId,0).
 */
int varmap_operand_id(VarMap *m, Operand op);

void varmap_init   (VarMap *m);
void varmap_destroy(VarMap *m);

#endif