/**
 * @file test_bucket.c
 * @brief Unit test per bucket.h/bucket.c (degree-indexed bucket list).
 *
 * Copre:
 *  - insert/remove di base e coerenza di inBucket[]/nonempty
 *  - pop_any_low restituisce sempre il grado minimo non vuoto (O(1) via ctz)
 *  - transizione di un nodo tra bucket diversi (simula il decremento di
 *    grado che avviene in ra_simplify quando un vicino viene rimosso)
 *  - pop su struttura vuota ritorna -1 senza side effect
 */

#include <stdio.h>
#include <assert.h>
#include "../bucket.h"

int main(void) {
    /* PASS 1: insert + pop_any_low sceglie il bucket di grado minimo */
    {
        Buckets b = buckets_create(/*nextVreg=*/10, /*k=*/14);
        bucket_insert(&b, 5, 3);
        bucket_insert(&b, 2, 1);
        bucket_insert(&b, 7, 1);
        bucket_insert(&b, 9, 0);

        int deg;
        int n = bucket_pop_any_low(&b, &deg);
        assert(n == 9 && deg == 0);
        bucket_remove(&b, n, deg);

        n = bucket_pop_any_low(&b, &deg);
        assert(deg == 1 && (n == 2 || n == 7));
        printf("PASS 1 ok: pop_any_low restituisce sempre il grado minimo.\n");
        buckets_free(&b);
    }

    /* PASS 2: rimozione di un nodo intermedio non rompe la lista collegata */
    {
        Buckets b = buckets_create(10, 14);
        bucket_insert(&b, 1, 2);
        bucket_insert(&b, 2, 2);
        bucket_insert(&b, 3, 2);   /* lista bucket[2]: 3 -> 2 -> 1 (prepend) */

        bucket_remove(&b, 2, 2);   /* rimuove il nodo centrale */
        assert(b.inBucket[2] == 0);
        assert(b.inBucket[1] == 1 && b.inBucket[3] == 1);

        /* la lista deve restare attraversabile: 3 -> 1 */
        assert(b.next[3] == 1);
        assert(b.prev[1] == 3);
        printf("PASS 2 ok: rimozione nodo intermedio preserva la lista.\n");
        buckets_free(&b);
    }

    /* PASS 3: spostamento di un nodo tra bucket (come fa ra_simplify quando
     * il grado di un vicino attivo scende) */
    {
        Buckets b = buckets_create(10, 14);
        bucket_insert(&b, 4, 5);
        assert(b.inBucket[4] == 1);
        assert((b.nonempty >> 5) & 1u);

        bucket_remove(&b, 4, 5);
        bucket_insert(&b, 4, 4);
        assert(!((b.nonempty >> 5) & 1u));   /* bucket 5 ora vuoto */
        assert((b.nonempty >> 4) & 1u);      /* bucket 4 ora popolato */

        int deg;
        int n = bucket_pop_any_low(&b, &deg);
        assert(n == 4 && deg == 4);
        printf("PASS 3 ok: transizione di grado sposta correttamente il nodo.\n");
        buckets_free(&b);
    }

    /* PASS 4: pop su struttura vuota ritorna -1, nessun crash */
    {
        Buckets b = buckets_create(10, 14);
        int deg = -99;
        int n = bucket_pop_any_low(&b, &deg);
        assert(n == -1);
        printf("PASS 4 ok: pop su bucket vuoto ritorna -1.\n");
        buckets_free(&b);
    }

    /* PASS 5: piu' nodi nello stesso bucket, tutti estraibili senza duplicati */
    {
        Buckets b = buckets_create(10, 14);
        bucket_insert(&b, 0, 0);
        bucket_insert(&b, 1, 0);
        bucket_insert(&b, 2, 0);

        int seen[3] = {0,0,0}, deg;
        for (int i = 0; i < 3; i++) {
            int n = bucket_pop_any_low(&b, &deg);
            assert(n >= 0 && deg == 0);
            assert(!seen[n]);
            seen[n] = 1;
            bucket_remove(&b, n, deg);
        }
        int deg2;
        assert(bucket_pop_any_low(&b, &deg2) == -1);
        printf("PASS 5 ok: tutti i nodi dello stesso bucket estratti senza duplicati.\n");
        buckets_free(&b);
    }

    printf("\nTutti i test bucket sono passati.\n");
    return 0;
}