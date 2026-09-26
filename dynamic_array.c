#include <stdlib.h>
#include <string.h>
#include "dynamic_array.h"
#include "arena.h"

void int_vector_init(IntVector *v, int capacity) {
    if (!v) return;

    v->arena = NULL;
    if (capacity <= 0) {
        v->data = NULL;
        v->len  = 0;
        v->cap  = 0;
        return;
    }

    v->data = malloc((size_t)capacity * sizeof(*v->data));
    if (!v->data) abort();
    v->len = 0;
    v->cap = capacity;
}

void int_vector_init_arena(IntVector *v, int capacity, Arena *arena) {
    if (!v) return;

    v->arena = arena;
    v->len   = 0;
    if (capacity <= 0 || !arena) {
        v->data = NULL;
        v->cap  = 0;
        return;
    }
    v->data = arena_alloc(arena, (size_t)capacity * sizeof(*v->data));
    v->cap  = capacity;
}

void int_vector_push(IntVector *v, int value) {
    if (!v) return;

    // Grow when full: double capacity (or start from INITIAL_CAP).
    if (__builtin_expect(v->len == v->cap, 0)) {
        int newCap = v->cap ? v->cap * 2 : INT_VECTOR_INITIAL_CAP;
        int *newData;
        // Arena cannot free old buffer; allocate a larger one.
        if (v->arena) {
            newData = arena_alloc(v->arena, (size_t)newCap * sizeof(*newData));
            if (v->data && v->len)
                memcpy(newData, v->data, (size_t)v->len * sizeof(*newData));
        } else {
            newData = realloc(v->data, (size_t)newCap * sizeof(*newData));
            if (!newData) abort();
        }
        v->data = newData;
        v->cap  = newCap;
    }

    v->data[v->len++] = value;
}

void int_vector_free(IntVector *v) {
    if (!v) return;
    // Only free when the vector owns its storage (not arena-backed).
    if (!v->arena)
        free(v->data);
    v->data  = NULL;
    v->len   = 0;
    v->cap   = 0;
    v->arena = NULL;
}

void int_vector_clear(IntVector *v) {
    if (!v) return;
    v->len = 0; // keep capacity and storage for reuse
}
