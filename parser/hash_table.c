#include "hash_table.h"

/* ── Forward declarations ───────────────────────────────────────────────── */

static Entry *create_entry(void *key, size_t keySize,
                            void *value, size_t valueSize,
                            unsigned long hash);

static int ht_resize(Hash_Table *table);

static int is_prime(size_t n);

static size_t next_prime(size_t n);

static inline void save_entry(Entry *e, FILE *f);

static inline int keys_equal(const void *a, size_t aSize, const void *b, size_t bSize);


/* ── API implementation ──────────────────────────────────────────────── */

Hash_Table *ht_create(size_t initialCapacity, hash_func hashFunction) {
    // Allocate memory for the main structure
    Hash_Table *table = malloc(sizeof(Hash_Table));
    if (!table) return NULL;

    table->size = 0;
    // Ensure capacity is at least the default value
    table->capacity = initialCapacity > 0 ? initialCapacity : HT_DEFAULT_CAPACITY;

    // Allocate bucket array and initialize with NULL pointers
    table->pool = calloc(table->capacity, sizeof(Entry *));
    if (!table->pool) {
        free(table); return NULL;
    }

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
    unsigned int index = h % table->capacity;

    for (Entry *e = table->pool[index]; e; e = e->next) {
        if (!keys_equal(e->key, e->keySize, key, keySize)) continue;

        void *newValue = malloc(valueSize);
        if (!newValue) return 0;
        free(e->value);
        e->value = newValue;
        memcpy(e->value, value, valueSize);
        e->size = valueSize;

        return 1;
    }

    if (table->size + 1 >= table->capacity) {
        if (!ht_resize(table)) return 0;
        index = h % table->capacity;
    }

    Entry *newEntry = create_entry(key, keySize, value, valueSize, h);
    if (!newEntry) return 0;

    newEntry->next = table->pool[index];
    table->pool[index] = newEntry;
    table->size++;

    return 1;
}

int ht_get(Hash_Table * restrict table, void * restrict key, size_t keySize,
           void * restrict destBuffer, size_t destSize) {
    if (!table || !key || !destBuffer) return 0;

    unsigned int index = table->hashFunction(key, keySize) % table->capacity;
    for (Entry *e = table->pool[index]; e; e = e->next) {
        if (!keys_equal(e->key, e->keySize, key, keySize)) continue;

        size_t n = e->size < destSize ? e->size : destSize;
        memcpy(destBuffer, e->value, n);
        return 1;
    }

    return 0;
}

int ht_delete(Hash_Table * restrict table, void * restrict key, size_t keySize) {
    if (!table || !key) return 0;

    unsigned int index = table->hashFunction(key, keySize) % table->capacity;
    Entry *prev = NULL;
    Entry *e = table->pool[index];

    while (e && !keys_equal(e->key, e->keySize, key, keySize)) {
        prev = e;
        e = e->next;
    }

    if (!e) return 0;

    if (prev) prev->next = e->next;
    else table->pool[index] = e->next;

    free(e->key);
    free(e->value);
    free(e);
    table->size--;

    return 1;
}

static int ht_resize(Hash_Table *table) {
    size_t  newCap  = next_prime(table->capacity * 2);
    Entry **newPool = calloc(newCap, sizeof(Entry *));
    if (!newPool) return 0;

    for (size_t i = 0; i < table->capacity; i++) {
        Entry *e = table->pool[i];
        while (e) {
            Entry *next = e->next;
            unsigned int newIndex = e->hash % newCap;

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

    for (size_t i = 0; i < table->capacity; i++) {
        Entry *e = table->pool[i];
        while (e) {
            Entry *next = e->next;
            free(e->key);
            free(e->value);
            free(e);
            e = next;
        }
    }

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

static Entry *create_entry(void *key, size_t keySize,
                            void *value, size_t valueSize,
                            unsigned long hash) {
    Entry *e = malloc(sizeof(Entry));
    if (!e) return NULL;
    e->key = e->value = NULL;

    e->key = malloc(keySize);
    if (!e->key) goto error;
    memcpy(e->key, key, keySize);
    e->keySize = keySize;

    e->value = malloc(valueSize);
    if (!e->value) goto error;
    memcpy(e->value, value, valueSize);

    e->size = valueSize;
    e->hash = hash;
    e->next = NULL;
    return e;

error:
    free(e->key);
    free(e->value);
    free(e);
    return NULL;
}

static int is_prime(size_t n) {
    if (n < 2) return 0;
    if (n == 2 || n == 3) return 1;
    if (n % 2 == 0 || n % 3 == 0) return 0;

    for (size_t i = 5; i * i <= n; i += 6) {
        if (n % i == 0 || n % (i + 2) == 0) return 0;
    }
    return 1;
}

static size_t next_prime(size_t n) {
    if (n < 2) return 2;
    if (n % 2 == 0) n++;
    while (!is_prime(n)) n += 2;
    return n;
}