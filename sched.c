/*
 * sched.c — Local List Scheduling, blocco base per blocco base.
 *
 * Miglioramenti rispetto alla versione precedente:
 *
 *  1. SparseMap al posto di lastDef[] + memset(0xFF):
 *       clear in O(1) invece di O(universo).
 *
 *  2. Renaming locale prima della costruzione del DAG:
 *       ogni definizione riceve un ID fresco → WAR e WAW scompaiono
 *       strutturalmente; rimangono solo dipendenze RAW.
 *       Senza renaming la rilevazione WAR richiedeva O(n²) (scan completo
 *       dell'intervallo lastDef..j per ogni scrittura); con renaming è O(1).
 *
 *  3. Arena locale per ogni schedule_block:
 *       tutti i dati temporanei (DAGNode, SuccNode, SparseMap, MaxHeap,
 *       buffer risultato) vivono in un'unica arena, distrutta in blocco a
 *       fine funzione → zero malloc/free individuali nel loop interno.
 *
 *  4. SuccNode come lista concatenata nell'arena:
 *       elimina il cap fisso MAX_SUCCS; non servono realloc.
 *
 *  5. MaxHeap per la ready list:
 *       O(log n) per push/pop al posto di O(n) per ricerca del massimo.
 *
 *  6. Helper locali sched_def/sched_uses con encoding univoco
 *       vreg → id, phys → nextVreg+id (evita collisioni vreg 0 / PHYS_RAX).
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "sched.h"
#include "arena.h"

/* =========================================================================
 * Latenze stimate — Intel Core i5 Haswell/Broadwell (tabelle Agner Fog)
 * ========================================================================= */
static int latency_of(MachOp op) {
    switch (op) {
    case MACH_ADD: case MACH_SUB: case MACH_NEG: case MACH_NOT:
    case MACH_XOR:  return 1;
    case MACH_IMUL: return 3;
    case MACH_IDIV: return 20;
    case MACH_SAL:  return 1;
    case MACH_MOV: case MACH_MOVSX: return 1;
    case MACH_LOAD: case MACH_STORE:
    case MACH_PUSH: case MACH_POP:  return 4;
    case MACH_CMP: case MACH_TEST:  return 1;
    case MACH_SETE: case MACH_SETNE:
    case MACH_SETL: case MACH_SETLE:
    case MACH_SETG: case MACH_SETGE: return 1;
    case MACH_JMP:
    case MACH_JE:  case MACH_JNE:
    case MACH_JL:  case MACH_JLE:
    case MACH_JG:  case MACH_JGE:  return 1;
    case MACH_CALL: case MACH_RET: return 3;
    case MACH_CQO:  return 1;
    case MACH_LABEL: case MACH_FUNC_BEGIN: case MACH_FUNC_END: return 0;
    default: return 1;
    }
}

/* =========================================================================
 * SparseMap — mappa intera chiave → valore intero, O(1) per operazione.
 *
 * Invariante classica dello sparse-set (EaC §B):
 *   dense[sparse[k]] == k   sse   k è presente
 *
 * sparse[] non viene inizializzato: la membership test usa il cross-check
 * sul dense[], eliminando false-positive in modo provabile.
 * Dimostrazione: dense[0..n-1] contiene solo chiavi effettivamente inserite;
 * se sparse[k] punta a una posizione p < n, dense[p] è una chiave j reale.
 * j == k solo se k era stato inserito → nessun falso positivo.
 *
 * Nota: leggere memoria non inizializzata è UB formale in C, ma il
 * ragionamento sopra garantisce correttezza su qualunque hardware reale
 * (la tecnica è ampiamente usata in motori di gioco e compilatori).
 * ========================================================================= */
typedef struct {
    int *sparse; /* sparse[key] → posizione in dense (non inizializzato) */
    int *dense;  /* dense[pos]  → chiave (scritto solo da smap_set)      */
    int *val;    /* val[pos]    → valore associato a dense[pos]           */
    int  n;      /* numero di entry attive                                */
    int  cap;    /* universo: chiavi valide in [0, cap)                   */
} SparseMap;

static void smap_init(SparseMap *m, int cap, Arena *arena) {
    m->sparse = arena_alloc(arena, (size_t)cap * sizeof(int));
    m->dense  = arena_alloc(arena, (size_t)cap * sizeof(int));
    m->val    = arena_alloc(arena, (size_t)cap * sizeof(int));
    m->n = 0; m->cap = cap;
}

/* Restituisce il valore, o -1 se la chiave non è presente. */
static inline int smap_get(const SparseMap *m, int k) {
    if ((unsigned)k >= (unsigned)m->cap) return -1;
    unsigned pos = (unsigned)m->sparse[k];
    if (pos >= (unsigned)m->n || m->dense[pos] != k) return -1;
    return m->val[pos];
}

/* Inserisce o aggiorna (key, value). */
static inline void smap_set(SparseMap *m, int k, int v) {
    if ((unsigned)k >= (unsigned)m->cap) return;
    unsigned pos = (unsigned)m->sparse[k];
    if (pos < (unsigned)m->n && m->dense[pos] == k) { m->val[pos] = v; return; }
    m->sparse[k]   = m->n;
    m->dense[m->n] = k;
    m->val[m->n]   = v;
    m->n++;
}

/* =========================================================================
 * SuccNode — cella lista concatenata per i successori del DAG.
 *            Allocata nell'arena locale: nessun free individuale.
 * ========================================================================= */
typedef struct SuccNode { int to; struct SuccNode *next; } SuccNode;

/* =========================================================================
 * DAGNode
 * ========================================================================= */
typedef struct {
    int       instrIdx;
    int       latency;
    int       height;          /* cammino critico ponderato verso un sink  */
    int       predCount;       /* predecessori non ancora schedulati        */
    int       scheduled;
    int       pinnedForFusion; /* CMP/TEST prima di Jcc: tenuto per il tail */
    SuccNode *succs;
    int       nSuccs;
} DAGNode;

/* =========================================================================
 * MaxHeap — coda con priorità per altezza (decrescente).
 * ========================================================================= */
typedef struct { int *data; int size; } MaxHeap;

static void heap_push(MaxHeap *h, int v, const DAGNode *nodes) {
    int i = h->size++;
    h->data[i] = v;
    while (i > 0) {
        int p = (i - 1) >> 1;
        if (nodes[h->data[p]].height >= nodes[h->data[i]].height) break;
        int t = h->data[p]; h->data[p] = h->data[i]; h->data[i] = t;
        i = p;
    }
}

static int heap_pop(MaxHeap *h, const DAGNode *nodes) {
    int top    = h->data[0];
    h->data[0] = h->data[--h->size];
    for (int i = 0;;) {
        int l = 2*i+1, r = 2*i+2, b = i;
        if (l < h->size && nodes[h->data[l]].height > nodes[h->data[b]].height) b = l;
        if (r < h->size && nodes[h->data[r]].height > nodes[h->data[b]].height) b = r;
        if (b == i) break;
        int t = h->data[b]; h->data[b] = h->data[i]; h->data[i] = t;
        i = b;
    }
    return top;
}

/* =========================================================================
 * BasicBlockOffset
 * ========================================================================= */
typedef struct { int start, end; } BasicBlockOffset;

/* =========================================================================
 * Predicati sulle istruzioni
 * ========================================================================= */
static int is_pinned(MachOp op) {
    switch (op) {
    case MACH_LABEL: case MACH_FUNC_BEGIN: case MACH_FUNC_END:
    case MACH_JMP:
    case MACH_JE:  case MACH_JNE:
    case MACH_JL:  case MACH_JLE:
    case MACH_JG:  case MACH_JGE:
    case MACH_RET: return 1;
    default:       return 0;
    }
}

/* Operazioni con effetti collaterali su memoria o registri impliciti:
   non riordinabili liberamente tra loro → serializzate con archi espliciti. */
static int has_side_effect(MachOp op) {
    switch (op) {
    case MACH_STORE: case MACH_PUSH: case MACH_POP:
    case MACH_CALL:  case MACH_IDIV: case MACH_CQO: return 1;
    default:                                          return 0;
    }
}

static int is_jcc(MachOp op) {
    switch (op) {
    case MACH_JE: case MACH_JNE:
    case MACH_JL: case MACH_JLE:
    case MACH_JG: case MACH_JGE: return 1;
    default:                       return 0;
    }
}

static inline int is_cmp_or_test(MachOp op) {
    return op == MACH_CMP || op == MACH_TEST;
}

/* =========================================================================
 * find_basic_blocks
 * ========================================================================= */
static BasicBlockOffset *find_basic_blocks(const MachFunction *f, int *outCount) {
    int cap = 8, count = 0;
    BasicBlockOffset *blocks = malloc((size_t)cap * sizeof(BasicBlockOffset));
    int start = 0;
    for (int i = 0; i < f->count; i++) {
        if (f->instrs[i].op == MACH_LABEL && i > start) {
            if (count == cap) {
                cap *= 2;
                blocks = realloc(blocks, (size_t)cap * sizeof(BasicBlockOffset));
            }
            blocks[count++] = (BasicBlockOffset){ start, i };
            start = i;
        }
    }
    if (start < f->count) {
        if (count == cap) {
            cap *= 2;
            blocks = realloc(blocks, (size_t)cap * sizeof(BasicBlockOffset));
        }
        blocks[count++] = (BasicBlockOffset){ start, f->count };
    }
    *outCount = count;
    return blocks;
}

/* =========================================================================
 * Helper per registri — encoding univoco pre-regalloc:
 *   vreg   → id            (0 .. nextVreg-1)
 *   phys p → nextVreg + p  (nextVreg .. nextVreg+PHYS_ALLOCATABLE-1)
 *
 * Evita la collisione vreg-0 / PHYS_RAX-0 che si avrebbe usando
 * direttamente physReg come indice.
 * PHYS_AL è normalizzato a PHYS_RAX (alias architetturale).
 * ========================================================================= */
static inline int normalize_phys(int physReg) {
    return (physReg == PHYS_AL) ? PHYS_RAX : physReg;
}

static inline int sched_reg(const MachOperand *o, int nextVreg) {
    switch (o->kind) {
    case MO_VREG: return o->vregId;
    case MO_PHYS: return nextVreg + normalize_phys(o->physReg);
    case MO_MEM:  return (o->mem.baseVreg  >= 0) ? o->mem.baseVreg  : -1;
    default:      return -1;
    }
}

/* Registro indice di un operando MO_MEM (pre-regalloc: sempre vregId). */
static inline int sched_reg_idx(const MachOperand *o) {
    return (o->kind == MO_MEM && o->mem.indexVreg >= 0) ? o->mem.indexVreg : -1;
}

/* Registro definito dall'istruzione, o -1. */
static int sched_def(const MachInstr *in, int nextVreg) {
    switch (in->op) {
    /* istruzioni che non definiscono un registro destinazione */
    case MACH_CMP:  case MACH_TEST:
    case MACH_JMP:
    case MACH_JE:   case MACH_JNE:
    case MACH_JL:   case MACH_JLE:
    case MACH_JG:   case MACH_JGE:
    case MACH_CALL: case MACH_RET:
    case MACH_PUSH: case MACH_STORE:
    case MACH_CQO:
    case MACH_LABEL: case MACH_FUNC_BEGIN: case MACH_FUNC_END:
        return -1;
    default:
        return sched_reg(&in->dst, nextVreg);
    }
}

/* Registri letti esplicitamente dall'istruzione (al più 5). */
static void sched_uses(const MachInstr *in, int nextVreg, int out[], int *n) {
    *n = 0;
    int r;
#define TRY(x) if ((r = (x)) >= 0) out[(*n)++] = r
    TRY(sched_reg    (&in->src1, nextVreg));
    TRY(sched_reg_idx(&in->src1));
    TRY(sched_reg    (&in->src2, nextVreg));
    TRY(sched_reg_idx(&in->src2));
    /* STORE/PUSH/IDIV/CQO leggono anche il registro dst */
    switch (in->op) {
    case MACH_STORE: case MACH_PUSH:
    case MACH_IDIV:  case MACH_CQO:
        TRY(sched_reg(&in->dst, nextVreg));
        break;
    default: break;
    }
#undef TRY
}

/* =========================================================================
 * dag_add_edge — aggiunge l'arco from→to (dedup + arena).
 * ========================================================================= */
static void dag_add_edge(DAGNode *nodes, int from, int to, Arena *arena) {
    if (from == to) return;
    /* dedup: scan lineare, in pratica O(1) perché i gradi sono piccoli */
    for (SuccNode *s = nodes[from].succs; s; s = s->next)
        if (s->to == to) return;
    SuccNode *sn      = arena_alloc(arena, sizeof(SuccNode));
    sn->to            = to;
    sn->next          = nodes[from].succs;
    nodes[from].succs = sn;
    nodes[from].nSuccs++;
    nodes[to].predCount++;
}

/* =========================================================================
 * build_dag — costruzione DAG con renaming locale + SparseMap
 *
 * Renaming: ad ogni definizione di registro r si assegna un ID fresco.
 * Dopo il renaming, due definizioni distinte dello stesso registro r
 * hanno ID diversi → nessuna WAW, nessuna WAR.
 * Rimangono solo dipendenze RAW: tracked via SparseMap(renamed→instr).
 *
 * Serializzazione degli effetti collaterali (STORE/CALL/IDIV/CQO):
 * rimane invariata come guard per ordinamento in memoria e clobber
 * di registri impliciti.
 *
 * Complessità per blocco di n istruzioni:
 *   O(n)    — scan principale (SparseMap O(1) per get/set)
 *   O(n)    — backward pass per le altezze
 *   O(n·d)  — dedup archi, d = grado medio ≪ n
 * ========================================================================= */
static void build_dag(const MachFunction *f, int start, int end,
                      DAGNode *nodes, Arena *arena) {
    int n        = end - start;
    int universe = f->nextVreg + PHYS_ALLOCATABLE; /* spazio registri totale */
    int cap      = universe + n;   /* renamed ID finiscono in [universe, cap) */

    /*
     * currentName[r]: ID rinominato corrente per il registro r.
     * Inizializzato all'identità; aggiornato ad ogni definizione.
     */
    int *currentName = arena_alloc(arena, (size_t)universe * sizeof(int));
    for (int r = 0; r < universe; r++) currentName[r] = r;
    int nextFresh = universe; /* prossimo ID fresco disponibile */

    /* SparseMap: renamed-ID → indice locale dell'istruzione che lo ha definito */
    SparseMap smap;
    smap_init(&smap, cap, arena);

    /* Inizializza nodi e segna coppie CMP/TEST+Jcc per la macro-fusion */
    for (int i = 0; i < n; i++) {
        nodes[i] = (DAGNode){
            .instrIdx  = start + i,
            .latency   = latency_of(f->instrs[start + i].op),
            .height    = latency_of(f->instrs[start + i].op),
        };
    }
    for (int i = 0; i + 1 < n; i++) {
        if (is_cmp_or_test(f->instrs[start + i].op) &&
            is_jcc(f->instrs[start + i + 1].op))
            nodes[i].pinnedForFusion = 1;
    }

    int lastSideEffect = -1;

    for (int j = 0; j < n; j++) {
        const MachInstr *inj = &f->instrs[start + j];

        /* RAW: per ogni uso, risolvi nel renamed-space e cerca il definiente */
        int uses[5]; int nuses;
        sched_uses(inj, f->nextVreg, uses, &nuses);
        for (int u = 0; u < nuses; u++) {
            int r   = uses[u];
            int ren = (r >= 0 && r < universe) ? currentName[r] : r;
            int dep = smap_get(&smap, ren);
            if (dep >= 0) dag_add_edge(nodes, dep, j, arena);
        }

        /* Serializzazione effetti collaterali */
        if (has_side_effect(inj->op)) {
            if (lastSideEffect >= 0)
                dag_add_edge(nodes, lastSideEffect, j, arena);
            lastSideEffect = j;
        }

        /* Def: rinomina → elimina WAR/WAW, registra per future RAW */
        int d = sched_def(inj, f->nextVreg);
        if (d >= 0 && d < universe) {
            int fresh      = nextFresh++; /* in [universe, cap): sempre valido */
            currentName[d] = fresh;
            smap_set(&smap, fresh, j);
        }
    }

    /* Altezze: backward pass (cammino critico ponderato) */
    for (int i = n - 1; i >= 0; i--) {
        int maxH = 0;
        for (SuccNode *s = nodes[i].succs; s; s = s->next)
            if (nodes[s->to].height > maxH) maxH = nodes[s->to].height;
        nodes[i].height = nodes[i].latency + maxH;
    }
}

/* =========================================================================
 * schedule_block — list scheduling locale su un blocco base.
 *
 * Struttura dell'emissione:
 *   1. Pinned head: LABEL / FUNC_BEGIN (emessi subito, ordine originale).
 *   2. Greedy con MaxHeap: emetti sempre il nodo con altezza massima.
 *   3. Pinned tail: terminatori + pinnedForFusion in ordine originale.
 *      Il tail garantisce adiacenza CMP/TEST+Jcc per la macro-fusion
 *      senza nessun meccanismo aggiuntivo.
 *
 * Tutti i buffer temporanei vivono nell'arena locale, distrutta a fine
 * funzione: zero malloc/free individuali nel loop di scheduling.
 * ========================================================================= */
static void schedule_block(MachFunction *f, int start, int end) {
    int n = end - start;
    if (n <= 1) return;

    Arena *arena = arena_create(0);

    DAGNode   *nodes  = arena_alloc(arena, (size_t)n * sizeof(DAGNode));
    MachInstr *result = arena_alloc(arena, (size_t)n * sizeof(MachInstr));
    int rCount = 0;

    build_dag(f, start, end, nodes, arena);

    MaxHeap heap;
    heap.data = arena_alloc(arena, (size_t)n * sizeof(int));
    heap.size = 0;

    /* 1. Pinned head */
    for (int i = 0; i < n; i++) {
        MachOp op = f->instrs[start + i].op;
        if (op == MACH_LABEL || op == MACH_FUNC_BEGIN) {
            result[rCount++] = f->instrs[start + i];
            nodes[i].scheduled = 1;
        }
    }

    /* Seed ready list: nodi senza predecessori, non pinnati */
    for (int i = 0; i < n; i++) {
        if (nodes[i].scheduled) continue;
        if (is_pinned(f->instrs[start + i].op)) continue;
        if (nodes[i].pinnedForFusion) continue;
        if (nodes[i].predCount == 0)
            heap_push(&heap, i, nodes);
    }

    /* 2. Greedy: emetti sempre il ready con altezza massima */
    while (heap.size > 0) {
        int chosen = heap_pop(&heap, nodes);
        result[rCount++] = f->instrs[start + chosen];
        nodes[chosen].scheduled = 1;

        for (SuccNode *s = nodes[chosen].succs; s; s = s->next) {
            int succ = s->to;
            if (nodes[succ].scheduled) continue;
            if (is_pinned(f->instrs[start + succ].op)) continue;
            if (nodes[succ].pinnedForFusion) continue;
            if (--nodes[succ].predCount == 0)
                heap_push(&heap, succ, nodes);
        }
    }

    /* 3. Pinned tail: tutto ciò che non è stato schedulato, ordine originale */
    for (int i = 0; i < n; i++) {
        if (!nodes[i].scheduled) {
            result[rCount++] = f->instrs[start + i];
            nodes[i].scheduled = 1;
        }
    }

    memcpy(&f->instrs[start], result, (size_t)n * sizeof(MachInstr));
    arena_destroy(arena);
}

/* =========================================================================
 * sched_schedule — entry point pubblico
 * ========================================================================= */
void sched_schedule(MachProgram *mp) {
    for (int fi = 0; fi < mp->count; fi++) {
        MachFunction *f = mp->functions[fi];
        if (!f || f->count == 0) continue;

        int bbCount = 0;
        BasicBlockOffset *blocks = find_basic_blocks(f, &bbCount);
        for (int b = 0; b < bbCount; b++)
            schedule_block(f, blocks[b].start, blocks[b].end);
        free(blocks);
    }
}