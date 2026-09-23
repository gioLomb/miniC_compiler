#include "varmap.h"
#include <stdlib.h>
#include <string.h>

/** Direct-mapped cache size; must be a power of 2. */
#define VARMAP_CACHE_SIZE 256

struct VarMap {
    Hash_Table *table;
    int         nextId;

    /* --- internal, key-based id cache (never stale, see file doc) ---
     * Slot = hash(key) & (VARMAP_CACHE_SIZE-1). Single-way, direct-mapped:
     * a lookup either hits (occupied slot, matching key) or falls through
     * to the hash table and unconditionally overwrites the slot — no
     * eviction policy needed. Not to be read/written by any code outside
     * varmap.c. */
    uint64_t cacheKeys[VARMAP_CACHE_SIZE];
    int      cacheIds[VARMAP_CACHE_SIZE];
    char     cacheOccupied[VARMAP_CACHE_SIZE];
};

/**
 * @brief 64-bit finalizer hash (splitmix64 / Murmur3 constants).
 *
 * The three XOR-shift + multiply rounds achieve full avalanche: every bit
 * of the input affects every bit of the output.  These specific constants
 * (0xff51afd7ed558ccd and 0xc4ceb9fe1a85ec53) are the standard Murmur3
 * 64-bit finalizer widely used in hash-map implementations precisely for
 * their excellent distribution over low-entropy integer keys — exactly the
 * kind of keys produced by varmap_makeKey (small kind, moderate a/b).
 *
 * @note  @p keySize is intentionally suppressed: VarMap keys are always
 *        exactly @c sizeof(uint64_t) bytes, so the parameter carries no
 *        information.  The @c (void) cast silences -Wunused-parameter.
 */
unsigned long varmap_hash(const void *key, size_t keySize) {
    (void)keySize;  // always sizeof(uint64_t); accepted for hash_func signature compatibility

    uint64_t rawKey = *(const uint64_t *)key;

    // three-round XOR-shift + multiply finalizer (Murmur3 / splitmix64)
    rawKey ^= rawKey >> 33; rawKey *= 0xff51afd7ed558ccdULL;
    rawKey ^= rawKey >> 33; rawKey *= 0xc4ceb9fe1a85ec53ULL;
    rawKey ^= rawKey >> 33;

    return (unsigned long)rawKey;
}

/**
 * @brief Pack (kind, a, b) into a single uint64_t for O(1) hash-table lookup.
 *
 * Bit layout:
 *   bits 63-62  →  kind  (2 bits : 0=VAR, 1=TEMP)
 *   bits 61-31  →  a     (31 bits: varLevel or tempId)
 *   bits 30-0   →  b     (31 bits: varOffset or 0)
 *
 * The AND masks are defensive: they prevent a large or negative input from
 * overwriting adjacent fields, keeping the encoding injective even when
 * the compiler or caller passes unexpected values.
 */
uint64_t varmap_makeKey(int kind, int level, int offset) {
    uint64_t k = 0;
    k |= (uint64_t)(kind & 0x3)        << 62;  // bits 63-62: operand category
    k |= (uint64_t)(level   & 0x7fffffff) << 31;  // bits 61-31: primary field
    k |= (uint64_t)(offset  & 0x7fffffff);         // bits 30-0:  secondary field
    return k;
}

/**
 * @brief Get-or-create: return the id for (kind, a, b), inserting it if absent.
 *
 * The fast path (lookup hits) returns immediately without touching nextId.
 * The slow path (first encounter) assigns the next sequential id so that
 * the id space stays dense — a requirement for safe bitset indexing.
 */
int varmap_mapToIndex(VarMap *m, int kind, int level, int offset) {
    uint64_t key = varmap_makeKey(kind, level, offset);

    // Direct-mapped cache probe: same hash as the backing table, so this
    // costs nothing extra to compute. A hit skips ht_get entirely.
    unsigned long h = varmap_hash(&key, sizeof key);
    size_t slot = (size_t)(h & (VARMAP_CACHE_SIZE - 1));

    if (m->cacheOccupied[slot] && m->cacheKeys[slot] == key)
        return m->cacheIds[slot];

    int id;
    if (!ht_get(m->table, &key, sizeof key, &id, sizeof id)) {
        // first encounter: assign next sequential id and persist it
        id = m->nextId++;
        ht_set(m->table, &key, sizeof key, &id, sizeof id);
    }

    // populate/overwrite the slot unconditionally (no eviction policy
    // needed for a single-way direct-mapped cache)
    m->cacheKeys[slot]     = key;
    m->cacheIds[slot]      = id;
    m->cacheOccupied[slot] = 1;

    return id;
}

/**
 * @brief Dispatch on Operand kind and delegate to varmap_mapToIndex().
 *
 * Only OPND_VAR and OPND_TEMP correspond to storage locations and are
 * assigned ids.  All other kinds (OPND_CONST_INT, OPND_CONST_FLOAT,
 * OPND_LABEL, OPND_FUNC, OPND_NONE) carry their value inline and are
 * returned as -1, signalling "no id" to callers such as liveness.c.
 */
int varmap_operand_id(VarMap *m, Operand op) {
    if (op.kind == OPND_VAR)  return varmap_mapToIndex(m, 0, op.data.varLevel, op.data.varOffset);
    if (op.kind == OPND_TEMP) return varmap_mapToIndex(m, 1, op.data.tempId, 0);
    return -1;
}

/**
 * @brief Create a VarMap with an empty hash table.
 *
 * Initial capacity 32 is deliberately small: most functions have far fewer
 * than 32 distinct variables/temporaries, so this avoids over-allocating
 * while the hash table's built-in resize handles larger functions.
 */
VarMap *varmap_create(void) {
    VarMap *m = malloc(sizeof(VarMap));
    if (!m) return NULL;
    *m = (VarMap){
        .table  = ht_create(32, varmap_hash),
        .nextId = 0,
    };
    return m;
}

int varmap_count(const VarMap *m) {
    return m->nextId;
}

void varmap_destroy(VarMap *m) {
    if (!m) return;
    ht_destroy(m->table, NULL);
    free(m);
}
