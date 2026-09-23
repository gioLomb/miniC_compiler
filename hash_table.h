#ifndef HASH_TABLE_H
#define HASH_TABLE_H


/**
 * @file hash_table.h
 * @brief Generic hash table with separate chaining and binary key/value support.
 *
 * Arena-backed open-addressing or chaining hash map that stores arbitrary
 * binary keys and values. Used for symbol tables, constant maps, value-
 * numbering tables and other associative data structures.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "arena.h"

#define MAX_KEY_LEN     (1 << 12)
#define MAX_VALUE_SIZE  (1 << 20)
#define HT_DEFAULT_CAPACITY 128   /* must stay a power of 2 */
#define HT_MAX_LOAD_NUM 3  
#define HT_MAX_LOAD_DEN 4

/* No 'seed' parameter: the hash function is deterministic, consistent
   with compiler use (same keys must always produce the same hash, run
   after run — also useful for debugging). */
typedef unsigned long (*hash_func)(const void *key, size_t keySize);

/**
 * @brief Callback for table traversal.
 */
typedef void (*ht_foreach_cb)(void *key, size_t keySize,
                               void *value, size_t valueSize,
                               void *userdata);

typedef struct Hash_Table Hash_Table;

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

/**
 * @brief Returns the number of entries currently stored in the table.
 *
 * @param table  Hash table instance (must not be NULL).
 * @return       Current entry count.
 */
size_t ht_size(const Hash_Table *table);

#endif
