#include <string.h>
#include <stdlib.h>
#include "symbol_table.h"

/**
 * @brief Deterministic FNV-1a hash algorithm for identifier lookup keys.
 */
static unsigned long symtab_hash(const void *key, size_t keySize) {
    const unsigned char *bytes = key;
    unsigned long h = 2166136261UL;
    for (size_t i = 0; i < keySize; i++) {
        h ^= bytes[i];
        h *= 16777619UL;
    }
    return h;
}

Scope *scope_create(Scope *parent) {
    // Allocate memory for new scope container
    Scope *scope = malloc(sizeof(Scope));
    if (!scope) return NULL;

    // Initialize local hash table
    scope->table = ht_create(SCOPE_DEFAULT_CAPACITY, symtab_hash);
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

Scope *scope_exit(Scope *scope) {
    // Return parent pointer without deallocating scope node
    return scope ? scope->parent : NULL;
}

int symtab_declare(Scope *scope, const char *name, const Symbol *sym) {
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

int symtab_lookup(Scope *scope, const char *name, Symbol *out) {
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

void symtab_destroy_tree(Scope *root) {
    if (!root) return;

    // Recursively destroy child sub-scopes in post-order traversal
    for (int i = 0; i < root->childCount; i++) {
        symtab_destroy_tree(root->children[i]);
    }

    // Free child pointer array, local table, and scope struct
    free(root->children);
    ht_destroy(root->table, NULL);
    free(root);
}