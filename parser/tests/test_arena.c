#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../arena.h"

int main(void) {
    /* PASS 1: arena_strdup/arena_alloc di base */
    Arena *a = arena_create(0);
    char *s1 = arena_strdup(a, "int x");
    if (strcmp(s1, "int x") != 0) { fprintf(stderr, "PASS 1 FALLITO\n"); return 1; }
    printf("PASS 1 ok: arena_strdup copia correttamente la stringa.\n");

    /* PASS 2: arena_sprintf costruisce esattamente la stringa richiesta,
       senza alcun limite scelto in anticipo */
    char *s2 = arena_sprintf(a, "%s %s", "float", "tasso_variazione");
    if (strcmp(s2, "float tasso_variazione") != 0) { fprintf(stderr, "PASS 2 FALLITO\n"); return 1; }
    printf("PASS 2 ok: arena_sprintf compone correttamente 'tipo nome'.\n");

    /* PASS 3: una stringa piu' lunga di qualunque vecchio buffer fisso
       (es. il "char combined[100]" del parser) non viene troncata */
    char longName[300];
    memset(longName, 'x', sizeof(longName) - 1);
    longName[sizeof(longName) - 1] = '\0';
    char *s3 = arena_sprintf(a, "int %s", longName);
    if (strlen(s3) != strlen("int ") + strlen(longName)) {
        fprintf(stderr, "PASS 3 FALLITO: lunghezza attesa %zu, ottenuta %zu\n",
                strlen("int ") + strlen(longName), strlen(s3));
        return 1;
    }
    printf("PASS 3 ok: un nome di 299 caratteri non viene troncato.\n");

    /* PASS 4: forza la crescita oltre il blocco iniziale (blockSize
       piccolo apposta) - verifica che arena_alloc continui a funzionare
       anche quando serve un nuovo blocco */
    Arena *small = arena_create(16);   /* blocco iniziale minuscolo */
    char *chunks[50];
    for (int i = 0; i < 50; i++) {
        chunks[i] = arena_sprintf(small, "elemento_%d", i);
    }
    int ok = 1;
    for (int i = 0; i < 50; i++) {
        char expected[32];
        snprintf(expected, sizeof(expected), "elemento_%d", i);
        if (strcmp(chunks[i], expected) != 0) { ok = 0; break; }
    }
    if (!ok) { fprintf(stderr, "PASS 4 FALLITO\n"); return 1; }
    printf("PASS 4 ok: crescita su piu' blocchi, ogni stringa resta leggibile.\n");
    arena_destroy(small);

    /* PASS 5: arena_strndup rispetta il limite richiesto */
    char *s5 = arena_strndup(a, "abcdef", 3);
    if (strcmp(s5, "abc") != 0) { fprintf(stderr, "PASS 5 FALLITO\n"); return 1; }
    printf("PASS 5 ok: arena_strndup taglia correttamente a 'n' caratteri.\n");

    arena_destroy(a);
    printf("\nTutti i test sono passati. Cleanup completato senza errori.\n");
    return 0;
}
