#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../arena.h"

int main(void) {
    /* Basic arena_strdup / arena_alloc */
    Arena *a = arena_create(0);
    char *s1 = arena_strdup(a, "int x");
    if (strcmp(s1, "int x") != 0) {
        fprintf(stderr, "PASS 1 FAILED\n");
        return 1;
    }
    printf("PASS 1 ok: arena_strdup copies the string correctly.\n");

    /* arena_sprintf builds the requested string with no fixed buffer limit */
    char *s2 = arena_sprintf(a, "%s %s", "float", "tasso_variazione");
    if (strcmp(s2, "float tasso_variazione") != 0) {
        fprintf(stderr, "PASS 2 FAILED\n");
        return 1;
    }
    printf("PASS 2 ok: arena_sprintf composes the string correctly.\n");

    /* Long string must not be truncated */
    char longName[300];
    memset(longName, 'x', sizeof(longName) - 1);
    longName[sizeof(longName) - 1] = '\0';
    char *s3 = arena_sprintf(a, "int %s", longName);
    if (strlen(s3) != strlen("int ") + strlen(longName)) {
        fprintf(stderr, "PASS 3 FAILED: expected length %zu, got %zu\n",
                strlen("int ") + strlen(longName), strlen(s3));
        return 1;
    }
    printf("PASS 3 ok: 299-character name is not truncated.\n");

    /* Force growth beyond the initial block */
    Arena *small = arena_create(16);
    char *chunks[50];
    for (int i = 0; i < 50; i++) {
        chunks[i] = arena_sprintf(small, "elemento_%d", i);
    }
    int ok = 1;
    for (int i = 0; i < 50; i++) {
        char expected[32];
        snprintf(expected, sizeof(expected), "elemento_%d", i);
        if (strcmp(chunks[i], expected) != 0) {
            ok = 0;
            break;
        }
    }
    if (!ok) {
        fprintf(stderr, "PASS 4 FAILED\n");
        return 1;
    }
    printf("PASS 4 ok: multi-block growth, every string remains readable.\n");
    arena_destroy(small);

    /* arena_strndup respects the requested limit */
    char *s5 = arena_strndup(a, "abcdef", 3);
    if (strcmp(s5, "abc") != 0) {
        fprintf(stderr, "PASS 5 FAILED\n");
        return 1;
    }
    printf("PASS 5 ok: arena_strndup truncates to n characters.\n");

    arena_destroy(a);
    printf("\nAll tests passed. Cleanup completed without errors.\n");
    return 0;
}
