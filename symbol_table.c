#include <string.h>
#include <stdlib.h>
#include "symbol_table.h"

/* ── Hash function (senza seed: deterministica) ─────────────────────────
 * FNV-1a: semplice, veloce, buona distribuzione per stringhe corte come
 * gli identificatori di un linguaggio sorgente. Nessun seed necessario:
 * non c'e' un avversario che sceglie apposta i nomi delle variabili per
 * far collidere la tabella. */
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
    Scope *scope = malloc(sizeof(Scope));
    if (!scope) return NULL;

    scope->table = ht_create(SCOPE_DEFAULT_CAPACITY, symtab_hash);
    if (!scope->table) {
        free(scope);
        return NULL;
    }
    scope->parent = parent;
    scope->children = NULL;
    scope->childCount = 0;
    scope->childCap = 0;
    scope->level = parent ? parent->level + 1 : 0;

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
    return scope ? scope->parent : NULL;
}

int symtab_declare(Scope *scope, const char *name, const Symbol *sym) {
    if (!scope || !name || !sym) return 0;

    size_t nameLen = strlen(name);
    if (nameLen == 0) return 0;

    /* Nessun limite superiore sulla lunghezza del nome: create_entry()
       in hash_table.c alloca esattamente keySize byte per la chiave,
       quindi non c'e' alcun vincolo strutturale da imporre qui. Un
       vecchio controllo "nameLen >= SYM_MAX_NAME_LEN" e' stato rimosso:
       confondeva silenziosamente "nome troppo lungo" con "gia'
       dichiarato" (stesso valore di ritorno 0), dando un messaggio
       d'errore fuorviante invece che accettare il nome com'e'. */

    /* Controlla SOLO lo scope corrente (non risale la catena): una
       variabile locale puo' legittimamente "nascondere" (shadow) una
       variabile con lo stesso nome in uno scope esterno - non e' un
       errore. E' un errore solo la ridichiarazione nello STESSO scope. */
    Symbol existing;
    if (ht_get(scope->table, (void *)name, nameLen, &existing, sizeof(existing))) {
        return 0;   /* gia' dichiarato in questo scope */
    }

    return ht_set(scope->table, (void *)name, nameLen, (void *)sym, sizeof(*sym));
}

int symtab_lookup(Scope *scope, const char *name, Symbol *out) {
    if (!name || !out) return 0;

    size_t nameLen = strlen(name);
    if (nameLen == 0) return 0;

    for (Scope *s = scope; s != NULL; s = s->parent) {
        if (ht_get(s->table, (void *)name, nameLen, out, sizeof(*out))) {
            return 1;
        }
    }
    return 0;   /* non trovato in nessuno scope della catena */
}

void symtab_destroy_tree(Scope *root) {
    if (!root) return;

    /* prima i figli (post-order): niente puntatori penzolanti se in
       futuro si aggiungesse logica che risale durante la distruzione */
    for (int i = 0; i < root->childCount; i++) {
        symtab_destroy_tree(root->children[i]);
    }

    free(root->children);
    ht_destroy(root->table, NULL);
    free(root);
}