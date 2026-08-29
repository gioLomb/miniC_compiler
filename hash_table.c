#include "hash_table.h"

/* ── Forward declarations ───────────────────────────────────────────────── */

static Entry *create_entry(Arena *arena, void *key, size_t keySize,
                            void *value, size_t valueSize,
                            unsigned long hash);

static int ht_resize(Hash_Table *table);

static inline void save_entry(Entry *e, FILE *f);

static inline int keys_equal(const void *a, size_t aSize, const void *b, size_t bSize);


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

    table->arena = arena_create(0);  // vita = vita della tabella
    table->hashFunction = hashFunction;
    return table;
}

void ht_snapshot(Hash_Table *table, const char *path) {
    if (!table || !path) return;

    FILE *f = fopen(path, "wb");
    if (!f) { perror("ht_snapshot: fopen"); return; }

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
        if (!keys_equal(e->key, e->keySize, key, keySize)) continue;

        if (valueSize <= e->cap) {
            // buffer already large enough (even if a previous update shrank
            // 'size' below 'cap'): overwrite in place, zero allocation
            memcpy(e->value, value, valueSize);
            e->size = valueSize;
        } else {
            // grown past the current physical buffer: allocate a new one,
            // rounded up to a power of 2 so a following moderate re-growth
            // reuses this same buffer instead of re-allocating again.
            // the old buffer stays "leaked" inside the arena (reclaimed in
            // bulk by ht_destroy, never before -- same tradeoff as before:
            // hash table lifetime here is always a single compiler pass)
            size_t newCap = round_pow2(valueSize);
            e->value = arena_alloc(table->arena, newCap);
            memcpy(e->value, value, valueSize);
            e->size = valueSize;
            e->cap  = newCap;
        }
        return 1;
    }

    if (table->size + 1 >= table->capacity) {
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
    // capacity always power of 2 (round_pow2 in ht_create/ht_resize):
    // h & (capacity-1) == h % capacity, no integer division needed
    unsigned int index = (unsigned int)(h & (table->capacity - 1));
    for (Entry *e = table->pool[index]; e; e = e->next) {
        // fast reject: differing cached hash implies differing keys,
        // skips the memcmp entirely on non-matching bucket entries
        if (e->hash != h) continue;
        if (!keys_equal(e->key, e->keySize, key, keySize)) continue;
        size_t n = e->size < destSize ? e->size : destSize;
        memcpy(destBuffer, e->value, n);
        return 1;
    }
    return 0;
}
/*
int ht_get(Hash_Table * restrict table, void * restrict key, size_t keySize,
           void * restrict destBuffer, size_t destSize) {
    if (!table || !key || !destBuffer) return 0;

    unsigned int index = (unsigned int)(table->hashFunction(key, keySize) & (table->capacity - 1));
    for (Entry *e = table->pool[index]; e; e = e->next) {
        if (!keys_equal(e->key, e->keySize, key, keySize)) continue;

        size_t n = e->size < destSize ? e->size : destSize;
        memcpy(destBuffer, e->value, n);
        return 1;
    }

    return 0;
}*/

int ht_delete(Hash_Table * restrict table, void * restrict key, size_t keySize) {
    if (!table || !key) return 0;

    unsigned int index = (unsigned int)(table->hashFunction(key, keySize) & (table->capacity - 1));
    Entry *prev = NULL;
    Entry *e = table->pool[index];

    while (e && !keys_equal(e->key, e->keySize, key, keySize)) {
        prev = e;
        e = e->next;
    }

    if (!e) return 0;

    if (prev) prev->next = e->next;
    else table->pool[index] = e->next;

    // no free(): memoria e' nell'arena, reclaimed in blocco da ht_destroy.
    // entry solo scollegata dalla catena bucket (leak fino a destroy, stesso
    // tradeoff di ht_set sopra).
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

static inline int keys_equal(const void *a, size_t aSize,
                             const void *b, size_t bSize) {
    return aSize == bSize && memcmp(a, b, aSize) == 0;
}

void ht_destroy(Hash_Table *table, const char *persistenceFilePath) {
    if (!table) return;

    if (persistenceFilePath) ht_snapshot(table, persistenceFilePath);

    // BONUS: prima serviva walk O(n) su ogni bucket con 3 free() per entry;
    // ora un solo arena_destroy libera tutto in blocco, O(1) rispetto a n
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
        key = NULL;
    }

clean:
    if (key) free(key);
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


/* ── Static helpers ──────────────────────────────────────────────────── */

static inline void save_entry(Entry *e, FILE *f) {
    fwrite(&e->keySize, sizeof(size_t), 1, f);
    fwrite(e->key,       e->keySize,    1, f);
    fwrite(&e->size,    sizeof(size_t), 1, f);
    fwrite(e->value,     e->size,       1, f);
}

static Entry *create_entry(Arena *arena, void *key, size_t keySize,
                            void *value, size_t valueSize,
                            unsigned long hash) {
    // arena_alloc mai NULL (arena.c fa exit(1) su OOM): niente goto error,
    // arena_alloc invece di malloc, bump allocation e' ~gratis
    Entry *e = arena_alloc(arena, sizeof(Entry));

    e->key = arena_alloc(arena, keySize);
    memcpy(e->key, key, keySize);
    e->keySize = keySize;

    // value buffer rounded up to a power of 2: a later update that grows
    // moderately (still <= cap) reuses this same buffer, see ht_set
    size_t cap = round_pow2(valueSize);
    e->value = arena_alloc(arena, cap);
    memcpy(e->value, value, valueSize);
    e->size = valueSize;
    e->cap  = cap;

    e->hash = hash;
    e->next = NULL;
    return e;
}
