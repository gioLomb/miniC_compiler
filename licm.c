#include <stdlib.h>
#include <string.h>
#include "licm.h"
#include "liveness.h"
#include "arena.h"

/* =========================================================================
 * PASSO 1 — Dominatori
 *
 * Dom[b] e' il bitset dei blocchi che dominano b.
 * Forward dataflow con intersezione:
 *
 *   Dom[0]   = {0}           (l'entry domina solo se stesso all'inizio)
 *   Dom[b≠0] = {tutti}       (pessimistico: ogni blocco domina tutti)
 *
 * Iterazione:
 *   Dom[b] = {b} ∪ ⋂ Dom[p] per ogni predecessore p di b
 *
 * Converge perche' i set non crescono mai (solo intersezioni).
 * Usiamo LiveSet/liveset_* di liveness.h come bitset generici.
 * ========================================================================= */

/* Calcola la matrice dei dominatori. Restituisce un array Dom[nBlocks]
 * allocato nell'arena. Dom[b] e' un LiveSet di block-ID. */
static LiveSet *computeDominators(IRFunction *f, int words, Arena *arena) {
    int n = f->blockCount;
    LiveSet *Dom = arena_alloc(arena, (size_t)n * sizeof(LiveSet));

    /* Inizializzazione */
    for (int b = 0; b < n; b++) {
        Dom[b] = liveset_new(arena, words);
        if (b == 0) {
            liveset_set(&Dom[b], 0);          /* Dom[0] = {0} */
        } else {
            /* Dom[b] = {tutti i blocchi} — riempi tutti i bit */
            memset(Dom[b].bits, 0xFF, (size_t)words * sizeof(uint64_t));
            /* azzera i bit oltre nBlocks per evitare falsi positivi */
            int leftover = n % 64;
            if (leftover) Dom[b].bits[words - 1] = (1ULL << leftover) - 1;
        }
    }

    /* Scratch: intersezione parziale */
    LiveSet inter = liveset_new(arena, words);
    LiveSet tmp   = liveset_new(arena, words);

    int changed = 1;
    while (changed) {
        changed = 0;
        for (int b = 1; b < n; b++) {
            /* inter = ⋂ Dom[p] su tutti i predecessori p */
            int firstPred = 1;
            for (int p = 0; p < n; p++) {
                for (int k = 0; k < 2; k++) {
                    if (f->blocks[p].succ[k] != b) continue;
                    if (firstPred) {
                        liveset_copy(&inter, &Dom[p]);
                        firstPred = 0;
                    } else {
                        /* inter = inter ∩ Dom[p] */
                        for (int w = 0; w < words; w++)
                            inter.bits[w] &= Dom[p].bits[w];
                    }
                }
            }
            if (firstPred) continue;   /* blocco irraggiungibile */

            /* Dom[b] = {b} ∪ inter */
            liveset_copy(&tmp, &inter);
            liveset_set(&tmp, b);

            if (!liveset_equal(&Dom[b], &tmp)) {
                liveset_copy(&Dom[b], &tmp);
                changed = 1;
            }
        }
    }
    return Dom;
}

/* Restituisce 1 se A domina B. */
static inline int dominates(LiveSet *Dom, int a, int b) {
    return liveset_test(&Dom[b], a);
}

/* =========================================================================
 * PASSO 2 — Rilevamento loop
 *
 * Un back-edge e' un arco B→H dove H domina B.
 * Il body del loop e' l'insieme di blocchi che possono raggiungere B
 * partendo da H senza passare per H stesso (BFS backward).
 * Le uscite sono archi da un blocco interno verso un blocco esterno.
 * ========================================================================= */

#define MAX_LOOPS 64   /* limite pratico per miniC */

typedef struct {
    int  header;          /* indice del blocco header */
    int  preHeader;       /* indice del pre-header (-1 se non ancora creato) */
    int *body;            /* array di indici di blocco nel loop body */
    int  bodyCount;
    int  exits[64];       /* blocchi di uscita (blocchi DENTRO il loop
                             con almeno un succ fuori dal loop) */
    int  exitCount;
} Loop;

/* BFS backward dal back-edge tail fino all'header per trovare il body. */
static void collectBody(IRFunction *f, int header, int tail,
                        int *body, int *bodyCount,
                        const int *predOf, /* predOf non serve — usiamo succ inverso */
                        Arena *arena) {
    int n = f->blockCount;
    char *inBody = arena_alloc(arena, (size_t)n);
    memset(inBody, 0, (size_t)n);

    /* Costruiamo i predecessori una volta sola */
    int *predCount = arena_alloc(arena, (size_t)n * sizeof(int));
    memset(predCount, 0, (size_t)n * sizeof(int));
    /* array di predecessori: max 2 per blocco */
    int (*preds)[2] = arena_alloc(arena, (size_t)n * 2 * sizeof(int));
    for (int b = 0; b < n; b++) preds[b][0] = preds[b][1] = -1;
    for (int b = 0; b < n; b++) {
        for (int k = 0; k < 2; k++) {
            int s = f->blocks[b].succ[k];
            if (s >= 0) {
                preds[s][predCount[s]++] = b;
            }
        }
    }

    /* BFS backward da tail a header */
    int *queue = arena_alloc(arena, (size_t)n * sizeof(int));
    int head = 0, tail_q = 0;

    inBody[header] = 1;   /* header fa parte del body */
    inBody[tail]   = 1;
    queue[tail_q++] = tail;

    while (head < tail_q) {
        int b = queue[head++];
        for (int k = 0; k < predCount[b]; k++) {
            int p = preds[b][k];
            if (p >= 0 && !inBody[p]) {
                inBody[p] = 1;
                queue[tail_q++] = p;
            }
        }
    }

    *bodyCount = 0;
    for (int b = 0; b < n; b++) {
        if (inBody[b]) body[(*bodyCount)++] = b;
    }
}

static int findLoops(IRFunction *f, LiveSet *Dom, Loop *loops, Arena *arena) {
    int n = f->blockCount;
    int nLoops = 0;

    int *body = arena_alloc(arena, (size_t)n * sizeof(int));

    for (int b = 0; b < n && nLoops < MAX_LOOPS; b++) {
        for (int k = 0; k < 2; k++) {
            int h = f->blocks[b].succ[k];
            if (h < 0) continue;
            if (!dominates(Dom, h, b)) continue;   /* non e' un back-edge */

            /* Back-edge trovato: b→h */
            Loop *L = &loops[nLoops++];
            L->header    = h;
            L->preHeader = -1;
            L->exitCount = 0;

            int bodyCount = 0;
            collectBody(f, h, b, body, &bodyCount, NULL, arena);

            L->body = arena_alloc(arena, (size_t)bodyCount * sizeof(int));
            L->bodyCount = bodyCount;
            memcpy(L->body, body, (size_t)bodyCount * sizeof(int));

            /* Uscite: blocchi del body con successore fuori dal body */
            char *inBody = arena_alloc(arena, (size_t)n);
            memset(inBody, 0, (size_t)n);
            for (int i = 0; i < bodyCount; i++) inBody[body[i]] = 1;

            for (int i = 0; i < bodyCount && L->exitCount < 64; i++) {
                int bl = body[i];
                for (int s = 0; s < 2; s++) {
                    int succ = f->blocks[bl].succ[s];
                    if (succ >= 0 && !inBody[succ]) {
                        /* Questo blocco e' un'uscita */
                        int already = 0;
                        for (int e = 0; e < L->exitCount; e++)
                            if (L->exits[e] == bl) { already = 1; break; }
                        if (!already) L->exits[L->exitCount++] = bl;
                        break;
                    }
                }
            }
        }
    }
    return nLoops;
}

/* =========================================================================
 * PASSO 3 — Pre-header
 *
 * Appende un nuovo IRBlock in coda a f->blocks con:
 *   succ[0] = header, succ[1] = -1
 * Reindirizza verso il pre-header tutti i predecessori ESTERNI al loop.
 * Aggiorna predCount.
 * ========================================================================= */
static int buildPreHeader(IRFunction *f, Loop *L) {
    int n       = f->blockCount;
    int header  = L->header;

    /* Espandi l'array dei blocchi se necessario */
    if (n == f->blockCap) {
        f->blockCap = f->blockCap ? f->blockCap * 2 : 16;
        f->blocks = realloc(f->blocks, (size_t)f->blockCap * sizeof(IRBlock));
    }

    int phIdx = f->blockCount++;
    IRBlock *ph = &f->blocks[phIdx];
    ph->start     = f->count;   /* blocco vuoto: start == end */
    ph->end       = f->count;
    ph->succ[0]   = header;
    ph->succ[1]   = -1;
    ph->predCount = 0;

    /* Predecessori esterni dell'header → pre-header */
    char *inBody = calloc((size_t)(phIdx + 1), 1);
    for (int i = 0; i < L->bodyCount; i++) inBody[L->body[i]] = 1;

    for (int b = 0; b < phIdx; b++) {
        for (int k = 0; k < 2; k++) {
            if (f->blocks[b].succ[k] == header && !inBody[b]) {
                f->blocks[b].succ[k] = phIdx;
                f->blocks[header].predCount--;
                ph->predCount++;
            }
        }
    }
    free(inBody);

    /* Il pre-header punta all'header */
    f->blocks[header].predCount++;
    L->preHeader = phIdx;
    return phIdx;
}

/* =========================================================================
 * PASSO 4 — Rilevamento invarianti con worklist
 * ========================================================================= */

static int isPure(IROp op) {
    switch (op) {
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_NEG: case IR_NOT:
    case IR_LT:  case IR_LE:  case IR_GT:  case IR_GE:  case IR_EQ: case IR_NE:
    case IR_ASSIGN:
        return 1;
    default: return 0;
    }
}

/* Per ogni variabile/temp tracciata nella VarMap, conta quante definizioni
 * esistono dentro il loop body. */
static int *countDefsInLoop(IRFunction *f, Loop *L, VarMap *vm, int numVars, Arena *arena) {
    int *defCount = arena_alloc(arena, (size_t)numVars * sizeof(int));
    memset(defCount, 0, (size_t)numVars * sizeof(int));

    for (int i = 0; i < L->bodyCount; i++) {
        int b = L->body[i];
        for (int j = f->blocks[b].start; j < f->blocks[b].end; j++) {
            IRInstr *in = &f->instrs[j];
            if (!liveness_defines_dst(in->op)) continue;
            if (!liveness_is_var_or_temp(in->dst.kind)) continue;
            int id = varmap_operand_id(vm, in->dst);
            if (id >= 0) defCount[id]++;
        }
    }
    return defCount;
}

/* Trova l'istruzione (indice in f->instrs) che definisce 'op' dentro il
 * loop e che e' gia' marcata invariante. Ritorna -1 se non esiste. */
static int singleLoopDefInvariant(IRFunction *f, Loop *L, Operand op,
                                   const char *invariant) {
    if (!liveness_is_var_or_temp(op.kind)) return -1;
    int found = -1;
    for (int i = 0; i < L->bodyCount; i++) {
        int b = L->body[i];
        for (int j = f->blocks[b].start; j < f->blocks[b].end; j++) {
            IRInstr *in = &f->instrs[j];
            if (!liveness_defines_dst(in->op)) continue;
            if (in->dst.kind != op.kind) continue;
            if (op.kind == OPND_VAR &&
                (in->dst.data.varLevel  != op.data.varLevel ||
                 in->dst.data.varOffset != op.data.varOffset)) continue;
            if (op.kind == OPND_TEMP && in->dst.data.tempId != op.data.tempId) continue;
            if (!invariant[j]) return -1;   /* esiste ma non e' invariante */
            found = j;
        }
    }
    return found;
}

/* Controlla se un operando sorgente e' invariante rispetto al loop:
 *   1. Costante letterale.
 *   2. Definito esclusivamente fuori dal loop (defCount == 0).
 *   3. Ha una sola definizione nel loop ed e' gia' marcata invariante. */
static int srcIsInvariant(IRFunction *f, Loop *L, Operand src,
                           const int *defCount, VarMap *vm,
                           const char *invariant) {
    if (src.kind == OPND_CONST_INT || src.kind == OPND_CONST_FLOAT || src.kind == OPND_NONE)
        return 1;
    if (!liveness_is_var_or_temp(src.kind)) return 0;
    int id = varmap_operand_id(vm, src);
    if (id < 0 || defCount[id] == 0) return 1;   /* definito fuori dal loop */
    if (defCount[id] == 1)
        return singleLoopDefInvariant(f, L, src, invariant) >= 0;
    return 0;
}

/* Worklist: marca le istruzioni invarianti e le propaga a cascata. */
static void findInvariants(IRFunction *f, Loop *L, VarMap *vm,
                            int numVars, const int *defCount,
                            char *invariant, Arena *arena) {
    int n = f->count;

    /* Worklist iniziale: tutte le istruzioni pure del body */
    int *worklist = arena_alloc(arena, (size_t)n * sizeof(int));
    int wHead = 0, wTail = 0;

    /* Mappa operand → lista di istruzioni che lo leggono (per propagazione
     * a cascata). Usiamo un approccio semplice: per ogni istruzione del
     * loop, registriamo i suoi src in una tabella inversa. */
    /* usedBy[varId] = lista di indici istruzione che leggono varId */
    int **usedBy    = arena_alloc(arena, (size_t)numVars * sizeof(int *));
    int  *usedByLen = arena_alloc(arena, (size_t)numVars * sizeof(int));
    int  *usedByCap = arena_alloc(arena, (size_t)numVars * sizeof(int));
    memset(usedBy,    0, (size_t)numVars * sizeof(int *));
    memset(usedByLen, 0, (size_t)numVars * sizeof(int));
    memset(usedByCap, 0, (size_t)numVars * sizeof(int));

    /* Costruisci usedBy e popola la worklist iniziale */
    for (int i = 0; i < L->bodyCount; i++) {
        int b = L->body[i];
        for (int j = f->blocks[b].start; j < f->blocks[b].end; j++) {
            IRInstr *in = &f->instrs[j];
            if (!isPure(in->op)) continue;
            if (!liveness_defines_dst(in->op)) continue;
            if (!liveness_is_var_or_temp(in->dst.kind)) continue;

            /* Registra questa istruzione come usante di src1 e src2 */
            Operand srcs[2] = { in->src1, in->src2 };
            for (int s = 0; s < 2; s++) {
                int id = varmap_operand_id(vm, srcs[s]);
                if (id < 0) continue;
                if (usedByLen[id] == usedByCap[id]) {
                    usedByCap[id] = usedByCap[id] ? usedByCap[id] * 2 : 4;
                    /* Alloca nuovo array nell'arena (non liberiamo il vecchio:
                       l'arena se ne occupa) */
                    int *newArr = arena_alloc(arena, (size_t)usedByCap[id] * sizeof(int));
                    if (usedByLen[id])
                        memcpy(newArr, usedBy[id], (size_t)usedByLen[id] * sizeof(int));
                    usedBy[id] = newArr;
                }
                usedBy[id][usedByLen[id]++] = j;
            }

            worklist[wTail++] = j;
        }
    }

    /* Elabora la worklist */
    while (wHead < wTail) {
        int j = worklist[wHead++];
        IRInstr *in = &f->instrs[j];

        if (!srcIsInvariant(f, L, in->src1, defCount, vm, invariant)) continue;
        if (!srcIsInvariant(f, L, in->src2, defCount, vm, invariant)) continue;

        if (invariant[j]) continue;   /* gia' marcata */
        invariant[j] = 1;

        /* Propaga a cascata: aggiungi alla worklist le istruzioni che
         * leggono il dst di questa istruzione */
        int dstId = varmap_operand_id(vm, in->dst);
        if (dstId >= 0) {
            for (int k = 0; k < usedByLen[dstId]; k++) {
                int dep = usedBy[dstId][k];
                if (!invariant[dep]) worklist[wTail++] = dep;
            }
        }
    }
}

/* =========================================================================
 * PASSO 5 — Safety check e movimento
 * ========================================================================= */

/* Controlla se il blocco 'blk' domina tutte le uscite del loop L. */
static int dominatesAllExits(Loop *L, LiveSet *Dom, int blk) {
    for (int i = 0; i < L->exitCount; i++) {
        if (!dominates(Dom, blk, L->exits[i])) return 0;
    }
    return 1;
}

/* Blocco a cui appartiene l'istruzione j. */
static int instrBlock(IRFunction *f, int j) {
    for (int b = 0; b < f->blockCount; b++) {
        if (j >= f->blocks[b].start && j < f->blocks[b].end) return b;
    }
    return -1;
}

/* Sposta nel pre-header le istruzioni invarianti e sicure.
 * Strategia: costruisce un nuovo array di istruzioni con:
 *   - le istruzioni del pre-header (inizialmente vuoto)
 *   - le istruzioni di tutti gli altri blocchi senza quelle spostate
 * Aggiorna start/end di tutti i blocchi. */
static int moveInvariants(IRFunction *f, Loop *L, LiveSet *Dom,
                           const char *invariant, const int *defCount,
                           VarMap *vm, LivenessResult *liv) {
    int nInstrs  = f->count;
    int nBlocks  = f->blockCount;
    int header   = L->header;
    int phIdx    = L->preHeader;
    int moved    = 0;

    /* Mappa vecchio indice → safe da spostare */
    char *doMove = calloc((size_t)nInstrs, 1);

    char *inBody = calloc((size_t)nBlocks, 1);
    for (int i = 0; i < L->bodyCount; i++) inBody[L->body[i]] = 1;

    for (int j = 0; j < nInstrs; j++) {
        if (!invariant[j]) continue;
        IRInstr *in = &f->instrs[j];

        int blk = instrBlock(f, j);
        if (!inBody[blk]) continue;

        /* Safety check 1: il blocco domina tutte le uscite */
        if (!dominatesAllExits(L, Dom, blk)) continue;

        /* Safety check 2: una sola definizione di dst nel loop */
        int dstId = varmap_operand_id(vm, in->dst);
        if (dstId < 0 || defCount[dstId] != 1) continue;

        /* Safety check 3: dst non e' live-in dell'header */
        if (liveset_test(&liv->LiveIn[header], dstId)) continue;

        doMove[j] = 1;
        moved++;
    }

    if (!moved) { free(doMove); free(inBody); return 0; }

    /* Costruisci nuovo array istruzioni:
     * [istr pre-header esistenti] [istr spostate] [istr non spostate] */
    IRInstr *newInstrs = malloc((size_t)(nInstrs + moved) * sizeof(IRInstr));
    int      newCount  = 0;

    /* Prima: le istruzioni del pre-header (era vuoto, ma per robustezza) */
    int phStart = f->blocks[phIdx].start;
    int phEnd   = f->blocks[phIdx].end;
    for (int j = phStart; j < phEnd; j++)
        newInstrs[newCount++] = f->instrs[j];

    /* Seconda: le istruzioni spostate (nell'ordine originale) */
    int phNewStart = 0;   /* il pre-header parte sempre da 0 nel nuovo array
                             solo se era il primo — usiamo un offset corretto */
    phNewStart = newCount - (phEnd - phStart);  /* inizio vecchie istr ph */
    int phMovedStart = newCount;
    for (int j = 0; j < nInstrs; j++) {
        if (doMove[j]) newInstrs[newCount++] = f->instrs[j];
    }
    int phNewEnd = newCount;

    /* Mappa vecchio indice → nuovo indice (per aggiornare start/end) */
    int *map = calloc((size_t)nInstrs, sizeof(int));
    for (int j = 0; j < nInstrs; j++) map[j] = -1;

    /* Terza: le istruzioni non spostate (tutti i blocchi tranne ph) */
    /* Prima le istruzioni precedenti al pre-header slot */
    for (int j = 0; j < nInstrs; j++) {
        if (j >= phStart && j < phEnd) continue;  /* gia' copiato */
        if (doMove[j]) continue;                   /* spostato nel ph */
        map[j] = newCount;
        newInstrs[newCount++] = f->instrs[j];
    }

    /* Aggiorna l'array istruzioni della funzione */
    free(f->instrs);
    f->instrs   = newInstrs;
    f->count    = newCount;
    f->capacity = newCount;

    /* Aggiorna start/end di tutti i blocchi */
    /* Il pre-header ottiene il range delle sue istruzioni (vecchie + spostate) */
    f->blocks[phIdx].start = phNewStart >= 0 ? phMovedStart : 0;
    f->blocks[phIdx].end   = phNewEnd;

    /* Sistemiamo: il pre-header era vuoto (phStart==phEnd), quindi
     * le sue "vecchie" istruzioni sono zero. Le istruzioni spostate
     * iniziano subito dopo le eventuali istruzioni pre-esistenti del ph. */
    f->blocks[phIdx].start = 0;   /* il ph e' il primo blocco nell'IR? No —
                                     usiamo il map per ricalcolare */

    /* Ricalcola start/end per ogni blocco tramite la map */
    for (int b = 0; b < nBlocks; b++) {
        if (b == phIdx) continue;
        int oldS = f->blocks[b].start, oldE = f->blocks[b].end;
        int newS = -1, newE = -1;
        for (int j = oldS; j < oldE; j++) {
            if (map[j] != -1) {
                if (newS == -1) newS = map[j];
                newE = map[j] + 1;
            }
        }
        f->blocks[b].start = (newS == -1) ? 0 : newS;
        f->blocks[b].end   = (newE == -1) ? 0 : newE;
    }

    /* Il pre-header: le sue istruzioni sono tutte nel range [phMovedStart, phNewEnd) */
    f->blocks[phIdx].start = phMovedStart;
    f->blocks[phIdx].end   = phNewEnd;

    free(doMove);
    free(inBody);
    free(map);
    return moved;
}

/* =========================================================================
 * Punto di ingresso
 * ========================================================================= */
int licm_optimize(IRFunction *f) {
    if (!f || f->blockCount == 0 || f->count == 0) return 0;

    int nBlocks = f->blockCount;
    Arena *arena = arena_create(0);

    /* Dimensione dei bitset in parole (ceil(nBlocks / 64)) */
    int words = (nBlocks + 63) / 64;

    /* PASSO 1: dominatori */
    LiveSet *Dom = computeDominators(f, words, arena);

    /* PASSO 2: trova i loop */
    Loop *loops = arena_alloc(arena, MAX_LOOPS * sizeof(Loop));
    int nLoops  = findLoops(f, Dom, loops, arena);
    if (nLoops == 0) { arena_destroy(arena); return 0; }

    /* Liveness per i safety check (LiveIn dell'header) */
    Arena *livArena = arena_create(0);
    LivenessResult liv = liveness_compute(f, NULL, livArena);

    int totalMoved = 0;

    for (int l = 0; l < nLoops; l++) {
        Loop *L = &loops[l];

        /* PASSO 3: pre-header */
        buildPreHeader(f, L);

        /* VarMap per questo loop (riusa quella della liveness) */
        VarMap *vm    = &liv.varMap;
        int numVars   = liv.numVars;

        /* Conta le definizioni dentro il loop */
        int *defCount = countDefsInLoop(f, L, vm, numVars, arena);

        /* PASSO 4: worklist per invarianti */
        char *invariant = arena_alloc(arena, (size_t)f->count);
        memset(invariant, 0, (size_t)f->count);
        findInvariants(f, L, vm, numVars, defCount, invariant, arena);

        /* PASSO 5: movimento sicuro nel pre-header */
        int moved = moveInvariants(f, L, Dom, invariant, defCount, vm, &liv);
        totalMoved += moved;

        /* Se abbiamo spostato istruzioni, ricalcola la liveness
         * (il CFG e' cambiato: il pre-header ha ora delle istruzioni) */
        if (moved) {
            varmap_destroy(&liv.varMap);
            arena_destroy(livArena);
            livArena = arena_create(0);
            liv = liveness_compute(f, NULL, livArena);
        }
    }

    varmap_destroy(&liv.varMap);
    arena_destroy(livArena);
    arena_destroy(arena);
    return totalMoved > 0;
}