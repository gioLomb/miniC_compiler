#ifndef BUCKET_H
#define BUCKET_H

#include <stdint.h>

/* Bucket per simplify (Briggs-optimistic), con estrazione O(1) tramite bitmask */
typedef struct {
    int     *head;       // head[d] = primo nodo nel bucket d, -1 se vuoto
    int     *bnext;      // prossimo nodo nella lista del bucket
    int     *bprev;      // precedente nodo
    int     *inBucket;   // flag: 1 se il nodo è attualmente in un bucket
    int      k;          // numero di bucket (di solito PHYS_ALLOCATABLE)
    uint32_t nonempty;   // bitmask: bit d = 1 se head[d] != -1
} Buckets;

/* Crea i bucket. 'nextVreg' = numero massimo di nodi (vreg).
   L'arena viene allocata internamente e distrutta da buckets_free(). */
Buckets buckets_create(int nextVreg, int k);

/* Distrugge i bucket e l'arena interna associata */
void buckets_free(Buckets *b);

/* Inserisce il nodo 'v' nel bucket di grado 'd' (presuppone 0 <= d < k) */
void bucket_insert(Buckets *b, int v, int d);

/* Rimuove il nodo 'v' dal bucket 'd' (deve trovarsi effettivamente in quel bucket) */
void bucket_remove(Buckets *b, int v, int d);

/* Estrae e rimuove il primo nodo dal bucket con indice più basso non vuoto.
   Restituisce il nodo rimosso, oppure -1 se tutti i bucket sono vuoti.
   In 'outD' viene scritto l'indice del bucket da cui è stato prelevato. */
int bucket_pop_any_low(Buckets *b, int *outD);

#endif