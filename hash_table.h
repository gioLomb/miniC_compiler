/**
 * @file hash_table.h
 * @brief Hash table generica con separate chaining e supporto a chiavi/valori binari.
 *
 * Versione ALLEGGERITA per uso interno single-thread (es. come mattone per
 * la symbol table di un compilatore): rispetto all'originale sono stati
 * rimossi il readers-writer lock (pthread_rwlock_t) e il seed anti hash-
 * flooding (generate_seed/lettura di /dev/urandom), inutili in un contesto
 * dove non c'e' concorrenza e le chiavi non provengono da input ostile.
 * L'API pubblica e la logica di collision/resize restano identiche.
 *
 * Ottimizzazioni applicate (vedi callgrind_report):
 *   1. Capacity della tabella sempre potenza di 2: index = h & (capacity-1)
 *      invece di h % capacity (modulo su primo). ht_get/ht_set sono gli
 *      hotspot #1 e #6 del profiling; l'AND elimina la divisione intera su
 *      ogni singola lookup/insert, in tutte le hash table del progetto
 *      (symtab, varmap, svn, cp). Le hash function usate (FNV-1a, splitmix64
 *      finalizer di varmap_hash) hanno avalanche sufficiente sui bit bassi:
 *      sicuro passare da modulo-primo ad AND-potenza-di-2.
 *   2. Entry.size (byte logici validi) separato da Entry.cap (byte fisici
 *      allocati nell'arena): un update che riduce e poi rialza la dimensione
 *      del valore riusa il buffer esistente invece di allocarne uno nuovo
 *      ogni volta che valueSize supera l'ultimo size registrato.
 */

#ifndef HASH_TABLE_H
#define HASH_TABLE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "arena.h"

#define MAX_KEY_LEN     (1 << 12)
#define MAX_VALUE_SIZE  (1 << 20)
#define HT_DEFAULT_CAPACITY 128   /* deve restare potenza di 2 */

/* Niente piu' parametro 'seed': la funzione di hash e' deterministica,
   coerente con l'uso da compilatore (le stesse chiavi devono sempre
   produrre lo stesso hash, run dopo run, utile anche per il debug). */
typedef unsigned long (*hash_func)(const void *key, size_t keySize);

/**
 * @brief Callback for table traversal.
 */
typedef void (*ht_foreach_cb)(void *key, size_t keySize,
                               void *value, size_t valueSize,
                               void *userdata);

typedef struct Entry {
    void *key;
    size_t keySize;
    void *value;
    size_t size;          /**< Logical byte count currently valid in @c value. */
    size_t cap;            /**< Physical byte count allocated for @c value (>= size). */
    unsigned long hash;
    struct Entry *next;
} Entry;

typedef struct {
    Entry **pool;
    size_t size;
    size_t capacity;        /**< Always a power of 2: index = h & (capacity - 1). */
    hash_func hashFunction;
    Arena *arena;   /* backs every Entry + key/value bytes: bump alloc,
                        no malloc/free per insert (vedi profiling) */
} Hash_Table;

/* ── General-purpose utility ────────────────────────────────────────────
 * Rounds n up to the next power of 2 (n itself if already a power of 2).
 * Not hash-table-specific: reusable anywhere a power-of-2 sized buffer or
 * table is wanted (e.g. a future dedicated VarMap open-addressing table).
 * Returns 1 for n <= 1.
 */
static inline size_t round_pow2(size_t n) {
    if (n <= 1) return 1;
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
#if SIZE_MAX > 0xFFFFFFFFu
    n |= n >> 32;
#endif
    return n + 1;
}

/* ── API ─────────────────────────────────────────────────────────────── */

/**
 * @brief Creates and initializes a new hash table.
 *
 * @c initialCapacity is rounded up to the next power of 2 internally
 * (capacity must stay a power of 2 for the AND-based index computation).
 *
 * @pre initialCapacity > 0 (recommended), hashFunction != NULL
 * @post A new Hash_Table instance is allocated and returned
 * @return Pointer to the new Hash_Table, or NULL on failure.
 */
Hash_Table *ht_create(size_t initialCapacity, hash_func hashFunction);

/**
 * @brief Saves a snapshot of the table to a binary file.
 */
void ht_snapshot(Hash_Table *table, const char *path);

/**
 * @brief Inserts or updates a key-value pair in the table.
 * @return 1 on success, 0 on failure
 */
int ht_set(Hash_Table * restrict table, void * restrict key, size_t keySize,
           void * restrict value, size_t valueSize);

/**
 * @brief Retrieves a copy of the value associated with a key.
 * @return 1 if found, 0 otherwise.
 */
int ht_get(Hash_Table * restrict table, void * restrict key, size_t keySize,
           void * restrict destBuffer, size_t destSize);

/**
 * @brief Removes an entry from the table.
 * @return 1 if found and deleted, 0 otherwise.
 */
int ht_delete(Hash_Table * restrict table, void * restrict key, size_t keySize);

/**
 * @brief Destroys the table and frees all allocated resources.
 * @param persistenceFilePath Optional path to save a final snapshot before destruction.
 */
void ht_destroy(Hash_Table *table, const char *persistenceFilePath);

/**
 * @brief Loads table content from a binary snapshot.
 * @return 1 on success, 0 on failure.
 */
int ht_load(Hash_Table *table, const char *path);

/**
 * @brief Iterates over every entry in the table.
 */
void ht_foreach(Hash_Table *table, ht_foreach_cb cb, void *userdata);

#endif
