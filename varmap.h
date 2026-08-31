#ifndef VARMAP_H
#define VARMAP_H

#include <stdint.h>
#include "ir.h"         /* Operand, OPND_VAR, OPND_TEMP */
#include "hash_table.h"

/**
 * @file varmap.h
 * @brief Compact operand-to-integer id mapping for IR analysis passes.
 *
 * VarMap assigns a small non-negative integer to each distinct IR operand
 * (variable or temporary) encountered in a function.  These ids are used
 * to index into dense bit-vector structures — primarily LiveSet in
 * liveness.h — so that dataflow sets can be stored and manipulated with
 * bitwise operations instead of symbol-table lookups.
 *
 * ### Why this is a separate module
 * Several IR passes need a compact operand map without running the full
 * liveness dataflow engine (SVN, SR, LICM each build their own).
 * Isolating VarMap here lets any pass that needs "give me an integer id
 * for this operand" use it independently of liveness.h.
 *
 * ### 64-bit key layout
 * Each (kind, a, b) triple is packed into a single uint64_t for O(1)
 * hash-table lookup:
 *
 *   bits 63-62 | bits 61-31 | bits 30-0
 *   -----------|------------|----------
 *   kind (2)   | a (31)     | b (31)
 *
 * | kind |  Operand type | a          | b          |
 * |------|---------------|------------|------------|
 * |  0   |  OPND_VAR     | varLevel   | varOffset  |
 * |  1   |  OPND_TEMP    | tempId     | 0          |
 *
 * The 31-bit masks ensure no field bleeds into its neighbour regardless of
 * the input values; kind fits in 2 bits because only 0 and 1 are used.
 */

/**
 * @brief Maps IR operands to compact non-negative integer ids.
 *
 * @c table   holds the packed-key → int mapping backed by Hash_Table.
 * @c nextId  is the next id to assign; starts at 0 and is incremented on
 *            every new insertion.  The id sequence is always dense
 *            [0, nextId), making it safe to use directly as an array or
 *            bitset index.
 */
typedef struct {
    Hash_Table *table;   /**< Hash table storing packed-key → id pairs. */
    int         nextId;  /**< Next id to hand out; equals number of distinct operands seen so far. */
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
