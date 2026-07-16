#ifndef ARENA_H
#define ARENA_H

#include <stddef.h>

/*
 * Arena allocator (bump/region allocator): la tecnica classica per la
 * gestione della memoria "usa e getta" di un compilatore. L'idea:
 *
 *   - si alloca un blocco grande una volta, e ogni arena_alloc() si
 *     limita a "spostare in avanti" un puntatore dentro quel blocco -
 *     nessuna malloc/free per ogni singola stringa temporanea;
 *   - non esiste un arena_free() per la singola allocazione: si libera
 *     TUTTO insieme con arena_destroy(), quando l'intera fase che usava
 *     quelle stringhe e' terminata.
 *
 * E' lo stesso principio gia' usato per l'albero di Scope (distrutto
 * tutto insieme con symtab_destroy_tree): qui si applica alle stringhe
 * temporanee che oggi vivono in buffer a dimensione fissa sparsi nel
 * parser e nei moduli Pass 1/semantica (es. "char combined[100]"),
 * ognuno con un limite scelto ad hoc e diverso dagli altri, a rischio
 * di troncamento silenzioso per un identificatore sufficientemente
 * lungo (snprintf non va in overflow, ma tronca senza dirlo).
 *
 * Con l'arena questi buffer scompaiono: arena_sprintf()/arena_strdup()
 * allocano ESATTAMENTE lo spazio che serve, quale che sia la lunghezza
 * reale della stringa - nessun numero magico da scegliere, nessun
 * troncamento possibile.
 */
typedef struct Arena Arena;

/*
 * Crea una nuova arena. 'blockSize' e' la dimensione (in byte) del
 * primo blocco sottostante allocato con malloc (0 = usa un default
 * sensato, 4096 byte). L'arena cresce automaticamente allocando altri
 * blocchi se il primo non basta (anche per singole richieste piu'
 * grandi del default): 'blockSize' e' solo un'indicazione di partenza,
 * non un limite massimo.
 */
Arena *arena_create(size_t blockSize);

/*
 * Alloca 'size' byte dentro l'arena (allineati a sizeof(void*)).
 * La memoria NON e' azzerata (comportamento come malloc, non calloc).
 */
void *arena_alloc(Arena *arena, size_t size);

/* Copia 's' (fino al terminatore) dentro l'arena. Come strdup, ma senza
   una malloc/free separata per ogni stringa. Ritorna NULL se s e' NULL. */
char *arena_strdup(Arena *arena, const char *s);

/* Copia al massimo i primi 'n' caratteri di 's' dentro l'arena,
   aggiungendo un terminatore. Come strndup, ma nell'arena. */
char *arena_strndup(Arena *arena, const char *s, size_t n);

/*
 * Costruisce una stringa formattata (stile printf) direttamente
 * nell'arena, con ESATTAMENTE lo spazio necessario per il risultato:
 * e' cio' che sostituisce ogni "char buf[N]; snprintf(buf, N, ...)"
 * sparso nel progetto — nessuna dimensione da indovinare in anticipo.
 */
char *arena_sprintf(Arena *arena, const char *fmt, ...);

/* Distrugge l'arena e TUTTA la memoria allocata al suo interno in un
   colpo solo (analogo a symtab_destroy_tree per l'albero di scope). */
void arena_destroy(Arena *arena);

#endif
