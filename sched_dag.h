#ifndef SCHED_DAG_H
#define SCHED_DAG_H

#include "instr_selector.h"

/* =========================================================================
 * SparseMap — mappa intera chiave → valore intero, O(1) per operazione.
 *
 * Invariante classica sparse-set (EaC §B):
 *   dense[sparse[k]] == k   sse   k è presente
 *
 * sparse[] non inizializzato: membership test usa cross-check su dense[],
 * eliminando false-positive. Lettura di memoria non inizializzata è UB
 * formale in C, ma corretto su qualunque hardware reale (tecnica standard
 * in motori di gioco e compilatori).
 * ========================================================================= */
typedef struct {
    int *sparse; /* sparse[key] → posizione in dense (non inizializzato) */
    int *dense;  /* dense[pos]  → chiave                                 */
    int *val;    /* val[pos]    → valore associato a dense[pos]           */
    int  n;      /* entry attive                                          */
    int  cap;    /* universo: chiavi valide in [0, cap)                   */
} SparseMap;

void smap_init(SparseMap *m, int cap);   /* usa arena interna del modulo */
int  smap_get (const SparseMap *m, int k);
void smap_set (SparseMap *m, int k, int v);

/* =========================================================================
 * MaxHeap — coda con priorità (altezza decrescente) per la ready list.
 * ========================================================================= */
typedef struct { int *data; int size; } MaxHeap;

/* Nodo del DAG delle dipendenze. */
typedef struct DAGNode {
    int       instrIdx;
    int       latency;
    int       height;          /* cammino critico ponderato verso un sink  */
    int       predCount;       /* predecessori non ancora schedulati        */
    int       scheduled;
    int       pinnedForFusion; /* CMP/TEST prima di Jcc: macro-fusion Intel */
    struct SuccNode *succs;
    int       nSuccs;
} DAGNode;

/* Lista concatenata dei successori (allocata nell'arena interna). */
typedef struct SuccNode { int to; struct SuccNode *next; } SuccNode;

/* Operazioni heap (nodes necessario per confronto priorità). */
void heap_push(MaxHeap *h, int v, const DAGNode *nodes);
int  heap_pop (MaxHeap *h, const DAGNode *nodes);

/* =========================================================================
 * Lifecycle arena interna del modulo.
 *
 * sched_dag_arena_init()  — crea l'arena una volta sola (chiamare prima
 *                           di qualunque build_dag).
 * sched_dag_arena_fini()  — distrugge l'arena (chiamare a fine programma
 *                           o dopo l'ultimo sched_schedule).
 *
 * build_dag chiama internamente arena_reset() prima di ogni costruzione:
 * la memoria viene riusata tra blocchi senza malloc/free aggiuntivi.
 * I SuccNode del blocco precedente diventano invalidi al reset, ma
 * schedule_block consuma ogni DAG completamente prima di chiamare
 * build_dag sul blocco successivo — nessun dangling pointer.
 * ========================================================================= */
void sched_dag_arena_init(void);
void sched_dag_arena_fini(void);

/* =========================================================================
 * build_dag — costruisce il DAG di dipendenze per il blocco [start, end).
 *
 * Chiama arena_reset() all'inizio: riusa la memoria dell'arena interna
 * senza deallocare/riallocare. Tutti i buffer temporanei (currentName,
 * SparseMap, SuccNode) vivono nell'arena interna del modulo.
 *
 * nodes[] è allocato dal caller (tipicamente nella propria arena locale).
 * ========================================================================= */
void build_dag(const MachFunction *f, int start, int end, DAGNode *nodes);

#endif /* SCHED_DAG_H */