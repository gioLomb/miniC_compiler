#include "hash_table.h"

typedef struct Entry {
    void *key;
    size_t keySize;
    void *value;
    size_t size;          /**< Logical byte count currently valid in @c value. */
    size_t cap;            /**< Physical byte count allocated for @c value (>= size). */
    unsigned long hash;
    struct Entry *next;
} Entry;

struct Hash_Table {
    Entry **pool;
    size_t size;
    size_t capacity;        /**< Always a power of 2: index = h & (capacity - 1). */
    hash_func hashFunction;
    Arena *arena;   /* backs every Entry + key/value bytes: bump alloc,
                        no malloc/free per insert (see profiling) */
};

/* ── Forward declarations ───────────────────────────────────────────────── */

static Entry *create_entry(Arena *arena, void *key, size_t keySize,
                            void *value, size_t valueSize,
                            unsigned long hash);

static int ht_resize(Hash_Table *table);

static inline void save_entry(Entry *e, FILE *f);

static inline int keys_equal(const void *a, size_t aSize, const void *b, size_t bSize);

/**
 * @brief Round @p n up to the next power of 2 (n itself if already one).
 *
 * Internal to this file: only ht_create() and create_entry() need it to
 * keep table capacity / value buffer capacity power-of-2-sized. Returns 1
 * for n <= 1.
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

/* ── API implementation ──────────────────────────────────────────────── */

Hash_Table *ht_create(size_t initialCapacity, hash_func hashFunction) {
    Hash_Table *table = malloc(sizeof(Hash_Table));
    if (!table) return NULL;

    table->size = 0;
    // capacity must stay a power of 2: index computation uses AND, not modulo
    size_t requested = initialCapacity > 0 ? initialCapacity : HT_DEFAULT_CAPACITY;
    table->capacity = round_pow2(requested);

    table->pool = calloc(table->capacity, sizeof(Entry *));
    if (!table->pool) {
        free(table);
        return NULL;
    }

    table->arena = arena_create(0);  // tied to the table's own lifetime
    table->hashFunction = hashFunction;
    return table;
}

void ht_snapshot(Hash_Table *table, const char *path) {
    if (!table || !path) return;

    FILE *f = fopen(path, "wb");
    if (!f) { perror("ht_snapshot: fopen"); return; }

    // dump every entry in every bucket, format matches ht_load()'s reader
    for (size_t i = 0; i < table->capacity; i++) {
        for (Entry *e = table->pool[i]; e; e = e->next)
            save_entry(e, f);
    }
    fclose(f);
}

int ht_set(Hash_Table * restrict table, void * restrict key, size_t keySize,
           void * restrict value, size_t valueSize) {
    if (!table || !key) return 0;

    unsigned long h = table->hashFunction(key, keySize);
    // capacity is a power of 2: h & (capacity-1) == h % capacity, no division
    unsigned int index = (unsigned int)(h & (table->capacity - 1));

    for (Entry *e = table->pool[index]; e; e = e->next) {
        if (e->hash != h || !keys_equal(e->key, e->keySize, key, keySize)) continue;

        if (valueSize > e->cap) {
            // outgrew the buffer: allocate a fresh one (old one just leaks
            // inside the arena, reclaimed in bulk by ht_destroy)
            size_t newCap = round_pow2(valueSize);
            e->value = arena_alloc(table->arena, newCap);
            e->cap  = newCap;
        }
        // existing buffer still fits: overwrite in place, no allocation
        memcpy(e->value, value, valueSize);
        e->size = valueSize;
        return 1;
    }
    if ((table->size + 1) * HT_MAX_LOAD_DEN >= table->capacity * HT_MAX_LOAD_NUM) {
        if (!ht_resize(table)) return 0;
        index = (unsigned int)(h & (table->capacity - 1));
    }

    Entry *newEntry = create_entry(table->arena, key, keySize, value, valueSize, h);
    newEntry->next = table->pool[index];
    table->pool[index] = newEntry;
    table->size++;

    return 1;
}

int ht_get(Hash_Table * restrict table, void * restrict key, size_t keySize,
           void * restrict destBuffer, size_t destSize) {
    if (!table || !key || !destBuffer) return 0;
    unsigned long h = table->hashFunction(key, keySize);
    unsigned int index = (unsigned int)(h & (table->capacity - 1));

    for (Entry *e = table->pool[index]; e; e = e->next) {
        if (e->hash == h && keys_equal(e->key, e->keySize, key, keySize)) {

            size_t n = e->size < destSize ? e->size : destSize;
            memcpy(destBuffer, e->value, n);
            return 1;
        }
    }
    return 0;
}

int ht_delete(Hash_Table * restrict table, void * restrict key, size_t keySize) {
    if (!table || !key) return 0;

    unsigned long h = table->hashFunction(key, keySize);
    unsigned int index = (unsigned int)(h & (table->capacity - 1));
    Entry *prev = NULL;
    Entry *e = table->pool[index];

    // walk the bucket's chain until the matching key or its end
    while (e) {
        if (e->hash == h && keys_equal(e->key, e->keySize, key, keySize)) {
            break;
        }
        prev = e;
        e = e->next;
    }

    if (!e) return 0;

    if (prev) prev->next = e->next;
    else table->pool[index] = e->next;

    // no free(): entry memory lives in the arena, reclaimed in bulk by
    // ht_destroy. Only unlinked from the bucket chain here (same
    // leak-until-destroy tradeoff as the reallocation branch in ht_set).
    table->size--;

    return 1;
}

static int ht_resize(Hash_Table *table) {
    // capacity is already a power of 2: doubling keeps that invariant,
    // no next_prime() search needed
    size_t  newCap  = table->capacity * 2;
    Entry **newPool = calloc(newCap, sizeof(Entry *));
    if (!newPool) return 0;

    size_t mask = newCap - 1;
    // rehash every existing entry into the larger pool (index depends on
    // capacity, so every entry must be re-bucketed)
    for (size_t i = 0; i < table->capacity; i++) {
        Entry *e = table->pool[i];
        while (e) {
            Entry *next = e->next;
            unsigned int newIndex = (unsigned int)(e->hash & mask);

            e->next = newPool[newIndex];
            newPool[newIndex] = e;
            e = next;
        }
    }

    free(table->pool);
    table->pool = newPool;
    table->capacity = newCap;
    return 1;
}

// byte-for-byte key comparison, size mismatch is an immediate reject
static inline int keys_equal(const void *a, size_t aSize,
                             const void *b, size_t bSize) {
    return aSize == bSize && memcmp(a, b, aSize) == 0;
}

void ht_destroy(Hash_Table *table, const char *persistenceFilePath) {
    if (!table) return;

    if (persistenceFilePath) ht_snapshot(table, persistenceFilePath);

    // single bulk free instead of walking every bucket/entry individually
    arena_destroy(table->arena);

    free(table->pool);
    free(table);
}

int ht_load(Hash_Table *table, const char *path) {
    if (!table || !path) return 0;

    if (table->size > 0) {
        fprintf(stderr, "ht_load: table is not empty, aborting\n");
        return 0;
    }

    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    size_t keyLen, valLen;
    char  *key = NULL;

    // binary format per entry (see save_entry): keyLen, key bytes,
    // valLen, value bytes — read and reinsert one entry per iteration
    while (fread(&keyLen, sizeof(size_t), 1, f) == 1) {
        if (keyLen == 0 || keyLen > MAX_KEY_LEN) {
            fprintf(stderr, "ht_load: invalid key length %zu, aborting\n", keyLen);
            break;
        }

        key = malloc(keyLen);
        if (!key) break;

        if (fread(key, 1, keyLen, f) != keyLen) goto clean;

        if (fread(&valLen, sizeof(size_t), 1, f) != 1) goto clean;
        if (valLen == 0 || valLen > MAX_VALUE_SIZE) {
            fprintf(stderr, "ht_load: invalid value length %zu, aborting\n", valLen);
            goto clean;
        }

        char *val = malloc(valLen);
        if (!val) goto clean;

        if (fread(val, 1, valLen, f) == valLen) {
            ht_set(table, key, keyLen, val, valLen);
        }

        free(val);
        free(key);
        key = NULL; // reset: only the aborted-mid-read case must free it below
    }

clean:
    if (key) free(key); // reached only via goto, on a read failure mid-entry
    fclose(f);
    return 1;
}

void ht_foreach(Hash_Table *table, ht_foreach_cb callback, void *userdata) {
    if (!table || !callback) return;

    for (size_t i = 0; i < table->capacity; i++) {
        for (Entry *e = table->pool[i]; e; e = e->next) {
            callback(e->key, e->keySize, e->value, e->size, userdata);
        }
    }
}

size_t ht_size(const Hash_Table *table) {
    return table->size;
}

/* ── Static helpers ──────────────────────────────────────────────────── */

// writes one entry as: keySize, key bytes, valueSize, value bytes
// (read back by the loop in ht_load)
static inline void save_entry(Entry *e, FILE *f) {
    fwrite(&e->keySize, sizeof(size_t), 1, f);
    fwrite(e->key,       e->keySize,    1, f);
    fwrite(&e->size,    sizeof(size_t), 1, f);
    fwrite(e->value,     e->size,       1, f);
}

static Entry *create_entry(Arena *arena, void *key, size_t keySize,
                            void *value, size_t valueSize,
                            unsigned long hash) {
    // arena_alloc never returns NULL (arena.c exits on OOM): no error path
    // needed here, unlike a malloc-based allocator
    Entry *e = arena_alloc(arena, sizeof(Entry));

    e->key = arena_alloc(arena, keySize);
    memcpy(e->key, key, keySize);
    e->keySize = keySize;

    // value buffer rounded up to a power of 2: a later moderate regrowth
    // (still <= cap) reuses this same buffer, see ht_set
    size_t cap = round_pow2(valueSize);
    e->value = arena_alloc(arena, cap);
    memcpy(e->value, value, valueSize);
    e->size = valueSize;
    e->cap  = cap;

    e->hash = hash;
    e->next = NULL;
    return e;
}
