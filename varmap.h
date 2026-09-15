#ifndef VARMAP_H
#define VARMAP_H


/**
 * @file varmap.h
 * @brief Compact operand-to-integer id mapping for IR analysis passes.
 *
 * Maps SSA-like virtual registers or temporary names to dense integer
 * identifiers, enabling efficient bit-set and array-based analyses such
 * as liveness, constant propagation and value numbering.
 */

#include "ir.h"         

#include <stdint.h>
#include "hash_table.h"

/** Direct-mapped cache size; must be a power of 2. */
#define VARMAP_CACHE_SIZE 256

typedef struct {
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
} VarMap;

/**
 * @brief Hash function for packed uint64_t keys, compatible with @c hash_func.
 *
 * Applies the splitmix64 / Murmur3 64-bit finalizer — three rounds of
 * XOR-shift followed by multiplication with well-chosen constants — to
 * produce a high-quality, avalanche-correct hash from a single 64-bit word.
 *
 * @p keySize is accepted only for compatibility with the @c hash_func
 * typedef in hash_table.h; it is always ignored because VarMap keys are
 * invariably @c sizeof(uint64_t) bytes.
 *
 * @param key      Pointer to a @c uint64_t key produced by varmap_makeKey().
 * @param keySize  Size of the key in bytes (always @c sizeof(uint64_t); ignored).
 * @return         Finalised 64-bit hash value truncated to @c unsigned long.
 */
unsigned long varmap_hash(const void *key, size_t keySize);

/**
 * @brief Pack a (kind, a, b) triple into a single uint64_t lookup key.
 *
 * Bit layout (see module header for full table):
 *   - bits 63-62: @p kind  (2 bits, masked to 0x3)
 *   - bits 61-31: @p a     (31 bits, masked to 0x7FFFFFFF)
 *   - bits 30-0 : @p b     (31 bits, masked to 0x7FFFFFFF)
 *
 * @param kind  Operand category: 0 = OPND_VAR, 1 = OPND_TEMP.
 * @param a     Primary discriminator: @c varLevel for variables,
 *              @c tempId for temporaries.
 * @param b     Secondary discriminator: @c varOffset for variables,
 *              0 for temporaries.
 * @return      Packed uint64_t key suitable for varmap_hash().
 */
uint64_t varmap_makeKey(int kind, int level, int offset);

/**
 * @brief Get or create the integer id for a (kind, a, b) triple.
 *
 * Performs a hash-table lookup.  If the entry already exists the stored id
 * is returned unchanged.  If not, a fresh id equal to @c m->nextId is
 * inserted and @c m->nextId is incremented.
 *
 * The get-or-create semantics guarantee that the same triple always maps to
 * the same id within the lifetime of @p m, making the id a stable bitset
 * index across multiple passes over the same instruction stream.
 *
 * @param m     VarMap to query or update.
 * @param kind  Operand category (0 = VAR, 1 = TEMP).
 * @param a     Primary field (varLevel or tempId).
 * @param b     Secondary field (varOffset or 0).
 * @return      Non-negative id in [0, m->nextId).
 */
int varmap_mapToIndex(VarMap *m, int kind, int level, int offset);

/**
 * @brief Map an IR Operand directly to its compact integer id.
 *
 * Convenience wrapper around varmap_mapToIndex() that extracts the (kind, a, b)
 * triple from an Operand according to its kind field:
 *   - @c OPND_VAR  → kind 0, a = varLevel,  b = varOffset
 *   - @c OPND_TEMP → kind 1, a = tempId,    b = 0
 *   - anything else (constants, labels, funcs) → @c -1 (not mapped)
 *
 * Constants and labels are never stored in the map because they carry
 * their value inline and do not correspond to a storage location.
 *
 * @param m   VarMap to query or update.
 * @param op  IR operand to resolve.
 * @return    Non-negative id if @p op is a variable or temporary, -1 otherwise.
 */
int varmap_operand_id(VarMap *m, Operand op);

/**
 * @brief Initialise an empty VarMap.
 *
 * Creates the backing hash table with an initial capacity of 32 buckets
 * — appropriate for the typical number of distinct variables/temporaries
 * per function — and resets @c nextId to 0.
 *
 * @pre  @p m points to an uninitialized VarMap struct.
 * @post @p m is ready to use; must be paired with a call to varmap_destroy().
 *
 * @param m  VarMap instance to initialise (must not be NULL).
 */
VarMap varmap_init();

/**
 * @brief Destroy a VarMap and free its backing hash table.
 *
 * After this call @p m is in an uninitialised state and must not be used
 * without a subsequent varmap_init().
 *
 * @param m  VarMap instance to destroy (must not be NULL).
 */
void varmap_destroy(VarMap *m);

#endif
