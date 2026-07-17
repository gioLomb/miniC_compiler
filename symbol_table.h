/**
 * @file symbol_table.h
 * @brief Symbol table a "sheaf of tables" (EaC cap. 5): una Hash_Table per
 * ogni scope lessicale, organizzate come un ALBERO (parent + children).
 *
 * Rispetto alla versione precedente (registro globale 'allScopes' separato
 * per il cleanup finale): qui non serve piu'. Ogni Scope conosce sia il
 * proprio genitore (per il lookup, che risale) sia i propri figli (per la
 * distruzione, che scende). L'albero e' l'unica fonte di verita': basta
 * un puntatore alla radice (lo scope globale) per raggiungere - e quindi
 * liberare - ogni scope mai creato, senza nessuna struttura duplicata.
 */

#ifndef SYMBOL_TABLE_H
#define SYMBOL_TABLE_H

#include <stdint.h>
#include "hash_table.h"

/* NOTA: questo valore NON e' (piu') un limite imposto da symtab_declare/
   symtab_lookup - la hash table sottostante accetta chiavi di qualunque
   lunghezza. Resta solo come dimensione suggerita per un eventuale
   buffer scratch di chi stampa/manipola un nome per proprio conto (es.
   un test); per costruire/spacchettare stringhe di lunghezza arbitraria
   durante la compilazione si usa invece l'arena allocator (arena.h). */
#define SYM_MAX_NAME_LEN   64
#define SYM_MAX_PARAMS     16   /* limite imposto da paramTypes: 32 bit / 2 bit per parametro */

/* Capacita' di default per uno scope: molto piu' piccola di quella di
   una tabella globale (HT_DEFAULT_CAPACITY = 101), dato che un singolo
   blocco/funzione ha tipicamente poche variabili/parametri. */
#define SCOPE_DEFAULT_CAPACITY 17

typedef enum {
    T_INT,
    T_FLOAT,
    T_VOID
} DataType;

typedef enum {
    SYM_VAR,
    SYM_FUNC
} SymbolKind;

/*
 * Symbol e' un tipo POD (Plain Old Data): nessun puntatore al suo interno.
 * E' un requisito, non uno stile: ht_set() copia il valore con un memcpy
 * shallow (vedi create_entry in hash_table.c). Tenendo tutti i campi a
 * dimensione fissa (interi/enum, niente puntatori), la hash table
 * generica puo' essere riusata cosi' com'e', senza modifiche per gestire
 * un "value" a lunghezza variabile o a proprieta'.
 *
 * paramTypes: i tipi dei parametri (per kind == SYM_FUNC) sono impacchettati
 * 2 bit ciascuno in un singolo uint32_t invece di un array di stringhe:
 * DataType ha solo 3 valori (T_INT/T_FLOAT/T_VOID), quindi 2 bit bastano
 * (0..3) e si risparmia spazio rispetto a un array [16][16] di char.
 * SYM_MAX_PARAMS = 16 e' il massimo rappresentabile in 32 bit / 2 bit.
 * Usa symtab_pack_param_type()/symtab_unpack_param_type() per leggerlo e
 * scriverlo invece di manipolare i bit a mano.
 */
typedef struct {
    SymbolKind kind;
    DataType   dataType;     /* per FUNC: tipo di ritorno */
    int        isArray;
    int        arraySize;

    /* Coordinate di risoluzione (shadowing): scopeLevel e' la profondita'
       lessicale dello Scope in cui questo Symbol e' stato dichiarato
       (= scope->level al momento della symtab_declare); offset e' la sua
       posizione nella tabella locale di quello scope (= scope->table->size
       prima dell'inserimento, non un contatore dedicato). Insieme
       identificano univocamente la variabile anche in presenza di
       shadowing, e vengono copiate cosi' come sono dentro il nodo AST
       corrispondente (vedi ASTNode in ast.h) al momento della dichiarazione
       o della risoluzione di un uso - simtab_lookup le restituisce gia'
       pronte, senza bisogno di risalire di nuovo la catena degli scope. */
    int        scopeLevel;
    int        offset;

    /* significativi solo se kind == SYM_FUNC */
    int        paramCount;
    uint32_t   paramTypes;   /* packed, 2 bit per parametro */

    /* metadati di debug, per messaggi di errore parlanti */
    int line;
    int col;
} Symbol;

/* Scrive il tipo del parametro 'index' (0-based) dentro 'paramTypes'. */
static inline void symtab_pack_param_type(uint32_t *paramTypes, int index, DataType type) {
    uint32_t shift = (uint32_t)(index * 2);
    *paramTypes &= ~(0x3u << shift);          /* azzera i 2 bit di quel parametro */
    *paramTypes |= ((uint32_t)type & 0x3u) << shift;
}

/* Legge il tipo del parametro 'index' (0-based) da 'paramTypes'. */
static inline DataType symtab_unpack_param_type(uint32_t paramTypes, int index) {
    uint32_t shift = (uint32_t)(index * 2);
    return (DataType)((paramTypes >> shift) & 0x3u);
}

/* Uno scope e' una Hash_Table piu' i puntatori a genitore e figli.
   La Hash_Table sottostante non sa nulla di questa gerarchia: e' compito
   di Scope incatenarle in entrambe le direzioni. */
typedef struct Scope {
    Hash_Table *table;
    struct Scope *parent;
    struct Scope **children;
    int childCount;
    int childCap;

    /* Profondita' lessicale (0 = scope globale/radice), scritta una sola
       volta in scope_create e mai piu' modificata. Serve a stampigliare
       Symbol.scopeLevel al momento della dichiarazione, senza dover
       ricalcolare nulla ad ogni lookup. */
    int level;
} Scope;

/* Crea un nuovo scope agganciato a 'parent' (NULL per lo scope globale,
   cioe' la radice dell'albero). Se parent non e' NULL, il nuovo scope
   viene automaticamente registrato tra i suoi figli, e 'level' e'
   impostato a parent->level + 1 (0 per la radice). */
Scope *scope_create(Scope *parent);

/* "Esce" dallo scope corrente restituendo il parent. NON distrugge la
   tabella (EaC: le tabelle chiuse restano utili per fasi successive/
   debug) - resta comunque raggiungibile dall'albero per il cleanup finale. */
Scope *scope_exit(Scope *scope);

/* Dichiara 'name' nello scope CORRENTE (non risale la catena):
   restituisce 0 se 'name' e' gia' dichiarato in questo stesso scope
   (redeclaration - errore semantico da segnalare al chiamante),
   altrimenti 1. */
int symtab_declare(Scope *scope, const char *name, const Symbol *sym);

/* Risolve 'name' risalendo la catena degli scope, dal piu' interno
   (scope) fino al piu' esterno. Se trovato, copia il Symbol in *out
   (scopeLevel/offset gia' pronti dentro, stampigliati alla dichiarazione)
   e restituisce 1; altrimenti restituisce 0 (nome non dichiarato). */
int symtab_lookup(Scope *scope, const char *name, Symbol *out);

/* Distrugge RICORSIVAMENTE l'intero albero di scope a partire da 'root'
   (tipicamente lo scope globale, radice dell'albero). Un'unica chiamata
   a fine compilazione libera ogni scope mai creato: l'albero stesso e'
   la struttura che tiene traccia di tutto, nessun registro esterno. */
void symtab_destroy_tree(Scope *root);

#endif
