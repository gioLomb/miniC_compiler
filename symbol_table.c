#include <string.h>
#include <stdlib.h>
#include "symbol_table.h"

struct Scope {
    Hash_Table *table;       /**< Local hash table mapping names to Symbol entries */
    struct Scope *parent;    /**< Pointer to enclosing outer scope (NULL for global root) */
    struct Scope **children; /**< Dynamic array of pointers to nested sub-scopes */
    int childCount;          /**< Number of active child scopes */
    int childCap;            /**< Allocated capacity for child scope pointer array */
    int level;               /**< Lexical nesting depth level (0 = global) */
};

/**
 * @brief Deterministic FNV-1a hash algorithm for identifier lookup keys.
 */
static unsigned long sym_hash(const void *key, size_t keySize) {
    const unsigned char *bytes = key;
    unsigned long h = 2166136261UL;
    size_t i = 0;

    // process 8 bytes/iteration via memcpy (safe for unaligned keys);
    // folds each word into the FNV accumulator with the same step as below
    for (; i + sizeof(uint64_t) <= keySize; i += sizeof(uint64_t)) {
        uint64_t word;
        memcpy(&word, bytes + i, sizeof(word));
        h ^= word;
        h *= 16777619UL;
    }
    // tail (<8 residual bytes): original byte-by-byte FNV-1a
    for (; i < keySize; i++) {
        h ^= bytes[i];
        h *= 16777619UL;
    }
    return h;
}
Scope *sym_scopeCreate(Scope *parent) {
    // Allocate memory for new scope container
    Scope *scope = malloc(sizeof(Scope));
    if (!scope) return NULL;

    // Initialize local hash table
    scope->table = ht_create(SCOPE_DEFAULT_CAPACITY, sym_hash);
    if (!scope->table) {
        free(scope);
        return NULL;
    }

    // Assign hierarchy pointers and calculate scope level depth
    scope->parent = parent;
    scope->children = NULL;
    scope->childCount = 0;
    scope->childCap = 0;
    scope->level = parent ? parent->level + 1 : 0;

    // Register new scope into parent's children array
    if (parent) {
        if (parent->childCount >= parent->childCap) {
            parent->childCap = (parent->childCap == 0) ? 4 : parent->childCap * 2;
            parent->children = realloc(parent->children, parent->childCap * sizeof(Scope *));
        }
        parent->children[parent->childCount++] = scope;
    }

    return scope;
}

Scope *sym_scopeExit(Scope *scope) {
    // Return parent pointer without deallocating scope node
    return scope ? scope->parent : NULL;
}

int sym_scope_level(const Scope *scope) {
    return scope ? scope->level : 0;
}

int sym_scope_count(const Scope *scope) {
    return scope ? ht_size(scope->table) : 0;
}

int sym_bind(Scope *scope, const char *name, const Symbol *sym) {
    // Guard against NULL input pointers
    if (!scope || !name || !sym) return 0;

    size_t nameLen = strlen(name);
    if (nameLen == 0) return 0;

    // Check current scope table for duplicate declaration error (allow outer shadowing)
    Symbol existing;
    if (ht_get(scope->table, (void *)name, nameLen, &existing, sizeof(existing))) {
        return 0; // Symbol already declared in current scope
    }

    // Insert copy of symbol into local scope table
    return ht_set(scope->table, (void *)name, nameLen, (void *)sym, sizeof(*sym));
}

int sym_resolve(Scope *scope, const char *name, Symbol *out) {
    if (!name || !out) return 0;

    size_t nameLen = strlen(name);
    if (nameLen == 0) return 0;

    // Walk up the scope parent chain starting from current scope
    for (Scope *s = scope; s != NULL; s = s->parent) {
        if (ht_get(s->table, (void *)name, nameLen, out, sizeof(*out))) {
            return 1; // Symbol successfully resolved
        }
    }
    return 0; // Symbol not found in any enclosing scope
}

void sym_finalize(Scope *root) {
    if (!root) return;

    // Recursively destroy child sub-scopes in post-order traversal
    for (int i = 0; i < root->childCount; i++) {
        sym_finalize(root->children[i]);
    }

    // Free child pointer array, local table, and scope struct
    free(root->children);
    ht_destroy(root->table, NULL);
    free(root);
}
