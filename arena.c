#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "arena.h"

#define ARENA_DEFAULT_BLOCK_SIZE 4096

/* Un blocco e' un'unica malloc: header + 'capacity' byte di dati subito
   dopo (flexible array member, C99). I blocchi restano incatenati in
   ordine di creazione a partire da 'head', solo per poterli liberare
   tutti in arena_destroy(); le allocazioni avvengono sempre nell'ultimo
   blocco della catena ('current'). */
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
    size_t remainder = n % a;
    return (remainder == 0) ? n : (n + a - remainder);
}

static ArenaBlock *blockCreate(size_t capacity) {
    ArenaBlock *block = (ArenaBlock *)malloc(sizeof(ArenaBlock) + capacity);
    if (!block) {
        fprintf(stderr, "arena: memoria esaurita (richiesti %zu byte)\n", capacity);
        exit(1);
    }
    block->next = NULL;
    block->capacity = capacity;
    block->used = 0;
    return block;
}

Arena *arena_create(size_t blockSize) {
    if (blockSize == 0) blockSize = ARENA_DEFAULT_BLOCK_SIZE;

    Arena *arena = (Arena *)malloc(sizeof(Arena));
    if (!arena) {
        fprintf(stderr, "arena: memoria esaurita\n");
        exit(1);
    }
    arena->defaultBlockSize = blockSize;
    arena->head = blockCreate(blockSize);
    arena->current = arena->head;
    return arena;
}

void *arena_alloc(Arena *arena, size_t size) {
    size_t aligned = alignUp(size);

    if (arena->current->capacity - arena->current->used < aligned) {
        /* Il blocco corrente non basta: ne creo uno nuovo. Se la
           richiesta stessa supera il blockSize di default (stringa
           enorme, caso raro), il nuovo blocco viene dimensionato
           esattamente su di essa invece che troncare o fallire. */
        size_t newCapacity = arena->defaultBlockSize;
        if (aligned > newCapacity) newCapacity = aligned;

        ArenaBlock *block = blockCreate(newCapacity);
        arena->current->next = block;
        arena->current = block;
    }

    void *ptr = arena->current->data + arena->current->used;
    arena->current->used += aligned;
    return ptr;
}

char *arena_strdup(Arena *arena, const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *copy = (char *)arena_alloc(arena, len + 1);
    memcpy(copy, s, len + 1);
    return copy;
}

char *arena_strndup(Arena *arena, const char *s, size_t n) {
    if (!s) return NULL;
    size_t len = strnlen(s, n);
    char *copy = (char *)arena_alloc(arena, len + 1);
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

    if (needed < 0) {
        va_end(args);
        return NULL;
    }

    char *buf = (char *)arena_alloc(arena, (size_t)needed + 1);
    vsnprintf(buf, (size_t)needed + 1, fmt, args);
    va_end(args);
    return buf;
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
