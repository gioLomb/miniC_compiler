/**
 * @file dynamic_array.c
 * @brief Resizable integer array — implementation.
 *
 * See dynamic_array.h for the module overview and public API documentation.
 */

#include <stdlib.h>
#include "dynamic_array.h"

void int_vector_init(IntVector *v, int capacity) {
    if (!v) return;

    if (capacity <= 0) {
        // Lazy path: no allocation until the first push arrives.
        v->data = NULL;
        v->len  = 0;
        v->cap  = 0;
        return;
    }

    // Eager path: single malloc at the requested capacity, skipping the
    // early doubling steps (4->8->16...) that int_vector_push would
    // otherwise perform.
    v->data = malloc((size_t)capacity * sizeof(*v->data));
    if (!v->data) abort(); // OOM: consistent with the compiler-wide policy
    v->len = 0;
    v->cap = capacity;
}

void int_vector_push(IntVector *v, int value) {
    if (!v) return;

    if (__builtin_expect(v->len == v->cap,0)) {
        // Double capacity, or fall back to the initial minimum when
        // starting from an empty (lazy-initialised) vector.
        int newCap = v->cap ? v->cap * 2 : INT_VECTOR_INITIAL_CAP;
        int *newData = realloc(v->data, (size_t)newCap * sizeof(*newData));
        if (!newData) abort(); // OOM: consistent with compiler-wide policy
        v->data = newData;
        v->cap  = newCap;
    }

    v->data[v->len++] = value;
}

void int_vector_free(IntVector *v) {
    if (!v) return;
    // Release the backing buffer and reset to the post-init state so the
    // vector can be safely reused or double-freed without side effects.
    free(v->data);
    v->data = NULL;
    v->len  = 0;
    v->cap  = 0;
}

void int_vector_clear(IntVector *v) {
    if (!v) return;
    v->len = 0; // Capacity retained so the next push sequence avoids realloc.
}