/**
 * @file dynamic_array.c
 * @brief Resizable integer array — implementation.
 *
 * See dynamic_array.h for the module overview and public API documentation.
 */

#include <stdlib.h>
#include "dynamic_array.h"

/** Initial capacity on the first push (avoids realloc on every early push). */
#define INT_VECTOR_INITIAL_CAP 4

void int_vector_init(IntVector *v) {
    if (!v) return;
    v->data = NULL;
    v->len  = 0;
    v->cap  = 0;
}

void int_vector_push(IntVector *v, int value) {
    if (!v) return;

    if (v->len == v->cap) {
        // double capacity, or use the initial minimum when starting from zero
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
    free(v->data);
    v->data = NULL;
    v->len  = 0;
    v->cap  = 0;
}

void int_vector_clear(IntVector *v) {
    if (!v) return;
    v->len = 0; // capacity retained so the next push sequence avoids realloc
}