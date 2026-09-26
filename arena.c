#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "arena.h"

/**
 * @brief Internal linked-list block representation.
 */
typedef struct ArenaBlock {
    struct ArenaBlock *next;
    size_t capacity;
    size_t used;
    char data[]; // Flexible array member for payload
} ArenaBlock;

/**
 * @brief Internal structure maintaining state for an arena allocation region.
 *
 * `bump`/`end` are the hot-path cursor: the common allocation is two
 * pointer compares and an add, not a walk of `current->next`.
 */
struct Arena {
    ArenaBlock *head;
    ArenaBlock *current;
    size_t defaultBlockSize;
    char *bump;
    char *end;
};

/**
 * @brief Aligns requested byte size upward to the target architecture's pointer alignment.
 */
static inline size_t align_up(size_t n) {
    size_t a = sizeof(void *);
    return (n + a - 1) & ~(a - 1);
}

static inline void arena_set_cursor(Arena *arena, ArenaBlock *block) {
    arena->current = block;
    arena->bump    = block->data + block->used;
    arena->end     = block->data + block->capacity;
}

/**
 * @brief Allocates a new physical memory block on heap containing header and payload space.
 */
static ArenaBlock *block_create(size_t capacity) {
    ArenaBlock *block = malloc(sizeof(ArenaBlock) + capacity);
    if (!block) {
        fprintf(stderr, "arena: out of memory (requested %zu bytes)\n", capacity);
        exit(1);
    }

    *block = (ArenaBlock){.next = NULL, .capacity = capacity, .used = 0};
    return block;
}

Arena *arena_create(size_t blockSize) {
    if (blockSize == 0) blockSize = ARENA_DEFAULT_BLOCK_SIZE;

    Arena *arena = malloc(sizeof(Arena));
    if (!arena) {
        fprintf(stderr, "arena: out of memory\n");
        exit(1);
    }

    arena->defaultBlockSize = blockSize;
    arena->head             = block_create(blockSize);
    arena_set_cursor(arena, arena->head);
    return arena;
}

static void *arena_alloc_slow(Arena *arena, size_t aligned) {
    while (arena->current->capacity - arena->current->used < aligned) {
        if (arena->current->next) {
            arena_set_cursor(arena, arena->current->next);
            continue;
        }

        size_t newCapacity = arena->defaultBlockSize;
        if (aligned > newCapacity) newCapacity = aligned;

        ArenaBlock *block = block_create(newCapacity);
        arena->current->next = block;
        arena_set_cursor(arena, block);
    }

    void *ptr = arena->bump;
    arena->bump += aligned;
    arena->current->used += aligned;
    return ptr;
}

void *arena_alloc(Arena *arena, size_t size) {
    size_t aligned = align_up(size);
    char *p = arena->bump;
    if (p + aligned <= arena->end) {
        arena->bump = p + aligned;
        arena->current->used += aligned;
        return p;
    }
    return arena_alloc_slow(arena, aligned);
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
    if (needed < 0) {
        va_end(args);
        return NULL;
    }

    char *buf = arena_alloc(arena, (size_t)needed + 1);
    vsnprintf(buf, (size_t)needed + 1, fmt, args);
    va_end(args);

    return buf;
}

void arena_reset(Arena *arena) {
    for (ArenaBlock *b = arena->head; b != NULL; b = b->next) {
        b->used = 0;
    }
    arena_set_cursor(arena, arena->head);
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
