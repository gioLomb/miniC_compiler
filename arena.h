#ifndef ARENA_H
#define ARENA_H

#include <stddef.h>

/*
 * Arena allocator (bump/region allocator).
 *
 * Modello base:
 *   - arena_alloc(): bump pointer dentro blocco corrente; nuovo blocco
 *     se non basta. Zero free individuali.
 *   - arena_destroy(): libera tutti i blocchi in un colpo.
 *
 * Estensione:
 *   - arena_reset(): azzera used su tutti i blocchi senza liberarli.
 *     Riusa la memoria già allocata per il ciclo successivo.
 *     TUTTI i puntatori restituiti da arena_alloc() precedenti diventano
 *     invalidi (il contenuto è sovrascritto alle prossime allocazioni).
 *     Uso tipico: arene "usa e riusa" dentro loop — stesse strutture
 *     riallocate ogni iterazione senza malloc/free.
 */
typedef struct Arena Arena;

Arena *arena_create (size_t blockSize);   /* 0 = default 4096 byte    */
void  *arena_alloc  (Arena *arena, size_t size);
char  *arena_strdup (Arena *arena, const char *s);
char  *arena_strndup(Arena *arena, const char *s, size_t n);
char  *arena_sprintf(Arena *arena, const char *fmt, ...);
void   arena_reset  (Arena *arena);       /* riusa, non libera         */
void   arena_destroy(Arena *arena);       /* libera tutto              */

#endif