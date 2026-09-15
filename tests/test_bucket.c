#include <stdio.h>
#include <assert.h>
#include "../bucket.h"

int main(void) {
    /* insert + pop_any_low returns the minimum-degree bucket */
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
        printf("PASS 1 ok: pop_any_low always returns the minimum degree.\n");
        buckets_free();
    }

    /* Removing a middle node does not break the linked list */
    {
        Buckets b = buckets_create(10, 14);
        bucket_insert(&b, 1, 2);
        bucket_insert(&b, 2, 2);
        bucket_insert(&b, 3, 2);

        bucket_remove(&b, 2, 2);
        assert(b.inBucket[2] == 0);
        assert(b.inBucket[1] == 1 && b.inBucket[3] == 1);
        assert(b.next[3] == 1);
        assert(b.prev[1] == 3);
        printf("PASS 2 ok: middle-node removal preserves the list.\n");
        buckets_free();
    }

    /* Moving a node between buckets (degree change) */
    {
        Buckets b = buckets_create(10, 14);
        bucket_insert(&b, 4, 5);
        assert(b.inBucket[4] == 1);
        assert((b.nonempty >> 5) & 1u);

        bucket_remove(&b, 4, 5);
        bucket_insert(&b, 4, 4);
        assert(!((b.nonempty >> 5) & 1u));
        assert((b.nonempty >> 4) & 1u);

        int deg;
        int n = bucket_pop_any_low(&b, &deg);
        assert(n == 4 && deg == 4);
        printf("PASS 3 ok: degree transition moves the node correctly.\n");
        buckets_free();
    }

    /* pop on empty structure returns -1 */
    {
        Buckets b = buckets_create(10, 14);
        int deg = -99;
        int n = bucket_pop_any_low(&b, &deg);
        assert(n == -1);
        printf("PASS 4 ok: pop on empty bucket returns -1.\n");
        buckets_free();
    }

    /* Multiple nodes in the same bucket are all extracted without duplicates */
    {
        Buckets b = buckets_create(10, 14);
        bucket_insert(&b, 0, 0);
        bucket_insert(&b, 1, 0);
        bucket_insert(&b, 2, 0);

        int seen[3] = {0, 0, 0}, deg;
        for (int i = 0; i < 3; i++) {
            int n = bucket_pop_any_low(&b, &deg);
            assert(n >= 0 && deg == 0);
            assert(!seen[n]);
            seen[n] = 1;
            bucket_remove(&b, n, deg);
        }
        int deg2;
        assert(bucket_pop_any_low(&b, &deg2) == -1);
        printf("PASS 5 ok: all nodes of the same bucket extracted without duplicates.\n");
        buckets_free();
    }

    printf("\nAll bucket tests passed.\n");
    return 0;
}
