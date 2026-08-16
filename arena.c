#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "arena.h"

#define ARENA_DEFAULT_BLOCK_SIZE 4096

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
 */
struct Arena {
    ArenaBlock *head;
    ArenaBlock *current;
    size_t defaultBlockSize;
};

/**
 * @brief Aligns requested byte size upward to the target architecture's pointer alignment.
 */
static size_t align_up(size_t n) {
    size_t a = sizeof(void *);
    return (n + a - 1) & ~(a - 1);
}

/**
 * @brief Allocates a new physical memory block on heap containing header and payload space.
 */
static ArenaBlock *block_create(size_t capacity) {
    // Allocate header size + raw payload capacity in a single malloc call
    ArenaBlock *block = malloc(sizeof(ArenaBlock) + capacity);
    if (!block) {
        fprintf(stderr, "arena: out of memory (requested %zu bytes)\n", capacity);
        exit(1);
    }
    block->next     = NULL;
    block->capacity = capacity;
    block->used     = 0;
    return block;
}

Arena *arena_create(size_t blockSize) {
    // Fallback to default block size if 0 is supplied
    if (blockSize == 0) blockSize = ARENA_DEFAULT_BLOCK_SIZE;

    Arena *arena = malloc(sizeof(Arena));
    if (!arena) {
        fprintf(stderr, "arena: out of memory\n");
        exit(1);
    }

    arena->defaultBlockSize = blockSize;
    arena->head             = block_create(blockSize);
    arena->current          = arena->head;
    return arena;
}

void *arena_alloc(Arena *arena, size_t size) {
    // Ensure all allocations align with standard alignment boundaries
    size_t aligned = align_up(size);

    // Check if the current block has enough capacity for requested size
    if (arena->current->capacity - arena->current->used < aligned) {
        // Expand with default size unless requested size exceeds default capacity
        size_t newCapacity = arena->defaultBlockSize;
        if (aligned > newCapacity) newCapacity = aligned;

        // Append new block to chain and advance active block pointer
        ArenaBlock *block = block_create(newCapacity);
        arena->current->next = block;
        arena->current       = block;
    }

    // Bump offset pointer forward and return calculated base pointer
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
    copy[len] = '\0'; // Ensure standard null-termination
    return copy;
}

char *arena_sprintf(Arena *arena, const char *fmt, ...) {
    va_list args, argsCopy;
    va_start(args, fmt);
    va_copy(argsCopy, args);

    // First pass: calculate required string buffer size without allocating
    int needed = vsnprintf(NULL, 0, fmt, argsCopy);
    va_end(argsCopy);
    if (needed < 0) {
        va_end(args);
        return NULL;
    }

    // Second pass: allocate exact required size in arena and format string
    char *buf = arena_alloc(arena, (size_t)needed + 1);
    vsnprintf(buf, (size_t)needed + 1, fmt, args);
    va_end(args);

    return buf;
}

void arena_reset(Arena *arena) {
    // Reset write pointers to 0 across all existing blocks to allow reuse
    for (ArenaBlock *b = arena->head; b; b = b->next) {
        b->used = 0;
    }
    // Set write target back to initial head block
    arena->current = arena->head;
}

void arena_destroy(Arena *arena) {
    if (!arena) return;

    // Traverse list and free all allocated physical blocks
    ArenaBlock *block = arena->head;
    while (block) {
        ArenaBlock *next = block->next;
        free(block);
        block = next;
    }

    // Free main struct container
    free(arena);
}
