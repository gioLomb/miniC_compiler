#ifndef DYNAMIC_ARRAY_H
#define DYNAMIC_ARRAY_H

/* Reusable dynamic array of int. */
typedef struct {
    int *data;
    int len;
    int cap;
} IntVector;

void int_vector_init(IntVector *v);
void int_vector_push(IntVector *v, int value);
void int_vector_free(IntVector *v);
void int_vector_clear(IntVector *v);

#endif
