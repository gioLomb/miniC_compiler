#ifndef ARENA_H
#define ARENA_H


/**
 * @file arena.h
 * @brief Fast arena (bump/region) memory allocator interface.
 */

#include <stddef.h>

#define ARENA_DEFAULT_BLOCK_SIZE 4096

typedef struct Arena Arena;

/**
 * @brief Creates a new arena allocator instance.
 *
 * @param blockSize Default size in bytes for internal memory blocks (pass 0 for default 4096 bytes).
 * @return Pointer to the newly initialized Arena instance.
 */
Arena *arena_create(size_t blockSize);

/**
 * @brief Allocates aligned memory from the arena.
 *
 * @param arena Pointer to the Arena instance.
 * @param size  Number of bytes to allocate.
 * @return Pointer to the allocated block of memory.
 */
void *arena_alloc(Arena *arena, size_t size);

/**
 * @brief Duplicates a null-terminated string into memory allocated from the arena.
 *
 * @param arena Pointer to the Arena instance.
 * @param s     The null-terminated string to copy.
 * @return Pointer to the newly allocated string copy, or NULL if `s` is NULL.
 */
char *arena_strdup(Arena *arena, const char *s);

/**
 * @brief Duplicates up to `n` characters of a string into memory allocated from the arena.
 *
 * @param arena Pointer to the Arena instance.
 * @param s     The string to copy.
 * @param n     Maximum number of characters to copy.
 * @return Pointer to the newly allocated, null-terminated string copy, or NULL if `s` is NULL.
 */
char *arena_strndup(Arena *arena, const char *s, size_t n);

/**
 * @brief Formats a string and allocates space for it within the arena.
 *
 * @param arena Pointer to the Arena instance.
 * @param fmt   Format string (printf-style).
 * @param ...   Additional arguments for formatting.
 * @return Pointer to the formatted, null-terminated string, or NULL on error.
 */
char *arena_sprintf(Arena *arena, const char *fmt, ...);

/**
 * @brief Resets the arena memory without freeing allocated memory blocks.
 *
 * Resets the allocation offset to zero on all blocks for reuse.
 * @note Invalidates all pointers previously allocated from this arena.
 *
 * @param arena Pointer to the Arena instance.
 */
void arena_reset(Arena *arena);

/**
 * @brief Destroys the arena and frees all associated heap memory blocks.
 *
 * @param arena Pointer to the Arena instance.
 */
void arena_destroy(Arena *arena);

#endif /* ARENA_H */
