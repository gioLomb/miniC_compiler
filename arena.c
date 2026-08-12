#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "arena.h"

#define ARENA_DEFAULT_BLOCK_SIZE 4096

typedef struct ArenaBlock {
    struct ArenaBlock *next;
    size_t capacity;
    size_t used;
    char data[];
} ArenaBlock;

struct Arena {
    ArenaBlock *head;
    ArenaBlock *current;
    size_t defaultBlockSize;
};

static size_t alignUp(size_t n) {
    size_t a = sizeof(void *);
    return (n + a - 1) & ~(a - 1);
}

static ArenaBlock *blockCreate(size_t capacity) {
    ArenaBlock *block = malloc(sizeof(ArenaBlock) + capacity);
    if (!block) {
        fprintf(stderr, "arena: memoria esaurita (richiesti %zu byte)\n", capacity);
        exit(1);
    }
    block->next     = NULL;
    block->capacity = capacity;
    block->used     = 0;
    return block;
}

Arena *arena_create(size_t blockSize) {
    if (blockSize == 0) blockSize = ARENA_DEFAULT_BLOCK_SIZE;
    Arena *arena = malloc(sizeof(Arena));
    if (!arena) { fprintf(stderr, "arena: memoria esaurita\n"); exit(1); }
    arena->defaultBlockSize = blockSize;
    arena->head             = blockCreate(blockSize);
    arena->current          = arena->head;
    return arena;
}

void *arena_alloc(Arena *arena, size_t size) {
    size_t aligned = alignUp(size);

    if (arena->current->capacity - arena->current->used < aligned) {
        size_t newCapacity = arena->defaultBlockSize;
        if (aligned > newCapacity) newCapacity = aligned;
        ArenaBlock *block = blockCreate(newCapacity);
        arena->current->next = block;
        arena->current       = block;
    }

    void *ptr = arena->current->data + arena->current->used;
    arena->current->used += aligned;
    return ptr;
}

char *arena_strdup(Arena *arena, const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *copy = arena_alloc(arena, len + 1);
    memcpy(copy, s, len + 1);
    return copy;
}

char *arena_strndup(Arena *arena, const char *s, size_t n) {
    if (!s) return NULL;
    size_t len = strnlen(s, n);
    char *copy = arena_alloc(arena, len + 1);
    memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}

char *arena_sprintf(Arena *arena, const char *fmt, ...) {
    va_list args, argsCopy;
    va_start(args, fmt);
    va_copy(argsCopy, args);

    int needed = vsnprintf(NULL, 0, fmt, argsCopy);
    va_end(argsCopy);
    if (needed < 0) { va_end(args); return NULL; }

    char *buf = arena_alloc(arena, (size_t)needed + 1);
    vsnprintf(buf, (size_t)needed + 1, fmt, args);
    va_end(args);
    return buf;
}

/*
 * arena_reset — riporta l'arena allo stato "vuota" senza liberare blocchi.
 *
 * Scorre tutti i blocchi e azzera used; current torna a head.
 * I blocchi allocati in eccesso rispetto a defaultBlockSize (quelli creati
 * per singole richieste grandi) restano nel pool e vengono riusati se la
 * richiesta successiva è abbastanza piccola da entrare nel blocco corrente.
 *
 * Complessità: O(numero di blocchi) — tipicamente O(1) se l'arena non è
 * mai cresciuta oltre il primo blocco.
 *
 * ATTENZIONE: tutti i puntatori ottenuti da arena_alloc() precedenti
 * diventano invalidi dopo questa chiamata.
 */
void arena_reset(Arena *arena) {
    for (ArenaBlock *b = arena->head; b; b = b->next)
        b->used = 0;
    arena->current = arena->head;
}

void arena_destroy(Arena *arena) {
    if (!arena) return;
    ArenaBlock *block = arena->head;
    while (block) {
        ArenaBlock *next = block->next;
        free(block);
        block = next;
    }
    free(arena);
}