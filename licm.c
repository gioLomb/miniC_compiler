#include <stdlib.h>
#include <string.h>
#include "licm.h"
#include "loop.h"
#include "liveness.h"
#include "arena.h"

/* ---- Purezza ----------------------------------------------------------- */
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

/* ---- Conta definizioni dentro il loop ---------------------------------- */
static int *countDefsInLoop(IRFunction *f, Loop *L, VarMap *vm,
                             int numVars, Arena *arena) {
    int *defCount = arena_alloc(arena, (size_t)numVars * sizeof(int));
    memset(defCount, 0, (size_t)numVars * sizeof(int));
    for (int i = 0; i < L->bodyCount; i++) {
        int b = L->body[i];
        for (int j = f->blocks[b].start; j < f->blocks[b].end; j++) {
            IRInstr *in = &f->instrs[j];
            if (!liveness_defines_dst(in->op) || !liveness_is_var_or_temp(in->dst.kind)) continue;
            int id = varmap_operand_id(vm, in->dst);
            if (id >= 0) defCount[id]++;
        }
    }
    return defCount;
}

/* ---- Invariant detection via worklist ---------------------------------- */

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
                (in->dst.data.varLevel != op.data.varLevel ||
                 in->dst.data.varOffset != op.data.varOffset)) continue;
            if (op.kind == OPND_TEMP && in->dst.data.tempId != op.data.tempId) continue;
            if (!invariant[j]) return -1;
            found = j;
        }
    }
    return found;
}

static int srcIsInvariant(IRFunction *f, Loop *L, Operand src,
                           const int *defCount, VarMap *vm, const char *invariant) {
    if (src.kind == OPND_CONST_INT || src.kind == OPND_CONST_FLOAT ||
        src.kind == OPND_NONE) return 1;
    if (!liveness_is_var_or_temp(src.kind)) return 0;
    int id = varmap_operand_id(vm, src);
    if (id < 0 || defCount[id] == 0) return 1;
    if (defCount[id] == 1) return singleLoopDefInvariant(f, L, src, invariant) >= 0;
    return 0;
}

/*
 * Struttura per la lista "usato da" di una variabile nella worklist.
 *
 * Bug fix: il vecchio codice usava arena_alloc + memcpy manuale per
 * simulare un realloc, abbandonando ogni volta il blocco precedente
 * nell'arena. Su loop con molte variabili e worklist grandi questo
 * causava uno spreco di memoria proporzionale al numero di raddoppi
 * effettuati, poiche' i blocchi arena abbandonati non vengono mai
 * recuperati fino alla distruzione dell'intera arena.
 *
 * Soluzione: usare malloc/realloc/free standard per usedBy, che
 * supportano il realloc vero e proprio senza sprechi. L'array viene
 * liberato esplicitamente con freeUsedBy() al termine di findInvariants.
 */
typedef struct {
    int *data;
    int  len;
    int  cap;
} UsedByList;

static void usedByAppend(UsedByList *list, int instrIdx) {
    if (list->len == list->cap) {
        list->cap = list->cap ? list->cap * 2 : 4;
        list->data = realloc(list->data, (size_t)list->cap * sizeof(int));
    }
    list->data[list->len++] = instrIdx;
}

static void freeUsedBy(UsedByList *usedBy, int numVars) {
    for (int i = 0; i < numVars; i++)
        free(usedBy[i].data);
}

static void findInvariants(IRFunction *f, Loop *L, VarMap *vm,
                            int numVars, const int *defCount,
                            char *invariant, Arena *arena) {
    int n = f->count;

    /* usedBy: per ogni variabile, lista degli indici delle istruzioni
     * del loop che la usano come sorgente. Allocato con malloc/realloc
     * (non arena) per poter usare realloc vero senza sprechi di memoria. */
    UsedByList *usedBy = calloc((size_t)numVars, sizeof(UsedByList));

    /* worklist: indici delle istruzioni candidate, allocata nell'arena
     * perche' la sua dimensione massima e' nota (f->count) e non cresce. */
    int *worklist = arena_alloc(arena, (size_t)n * sizeof(int));
    int wHead = 0, wTail = 0;

    for (int i = 0; i < L->bodyCount; i++) {
        int b = L->body[i];
        for (int j = f->blocks[b].start; j < f->blocks[b].end; j++) {
            IRInstr *in = &f->instrs[j];
            if (!isPure(in->op) || !liveness_defines_dst(in->op) ||
                !liveness_is_var_or_temp(in->dst.kind)) continue;

            /* Registra le dipendenze src -> j per la propagazione */
            Operand srcs[2] = { in->src1, in->src2 };
            for (int s = 0; s < 2; s++) {
                int id = varmap_operand_id(vm, srcs[s]);
                if (id >= 0) usedByAppend(&usedBy[id], j);
            }
            worklist[wTail++] = j;
        }
    }

    while (wHead < wTail) {
        int j = worklist[wHead++];
        IRInstr *in = &f->instrs[j];
        if (!srcIsInvariant(f, L, in->src1, defCount, vm, invariant)) continue;
        if (!srcIsInvariant(f, L, in->src2, defCount, vm, invariant)) continue;
        if (invariant[j]) continue;
        invariant[j] = 1;
        int dstId = varmap_operand_id(vm, in->dst);
        if (dstId >= 0) {
            for (int k = 0; k < usedBy[dstId].len; k++) {
                int dep = usedBy[dstId].data[k];
                if (!invariant[dep])
                    worklist[wTail++] = dep;
            }
        }
    }

    freeUsedBy(usedBy, numVars);
    free(usedBy);
}

/* ---- Safety check e movimento ----------------------------------------- */

static int dominatesAllExits(Loop *L, LiveSet *Dom, int blk) {
    for (int i = 0; i < L->exitCount; i++)
        if (!loop_dominates(Dom, blk, L->exits[i])) return 0;
    return 1;
}

static int instrBlock(IRFunction *f, int j) {
    for (int b = 0; b < f->blockCount; b++)
        if (j >= f->blocks[b].start && j < f->blocks[b].end) return b;
    return -1;
}

/*
 * Sposta le istruzioni invarianti nel pre-header.
 *
 * Inserimento allineato a SR: le istruzioni hoistate vengono posizionate
 * nell'array lineare IMMEDIATAMENTE PRIMA dell'header del loop
 * (insertAt = f->blocks[header].start), non in posizione 0.
 * In questo modo l'ordine fisico e': [pre-loop] [invarianti] [header...],
 * coerente con l'ordine di esecuzione logico e con quanto fa SR.
 */
static int moveInvariants(IRFunction *f, Loop *L, LiveSet *Dom,
                           const char *invariant, const int *defCount,
                           VarMap *vm, LivenessResult *liv) {
    int nInstrs = f->count, nBlocks = f->blockCount;
    int header  = L->header, phIdx = L->preHeader;
    int moved   = 0;

    char *doMove = calloc((size_t)nInstrs, 1);
    char *inBody = calloc((size_t)nBlocks, 1);
    for (int i = 0; i < L->bodyCount; i++) inBody[L->body[i]] = 1;

    for (int j = 0; j < nInstrs; j++) {
        if (!invariant[j]) continue;
        int blk = instrBlock(f, j);
        if (!inBody[blk]) continue;
        if (!dominatesAllExits(L, Dom, blk)) continue;
        int dstId = varmap_operand_id(vm, f->instrs[j].dst);
        if (dstId < 0 || defCount[dstId] != 1) continue;
        if (liveset_test(&liv->LiveIn[header], dstId)) continue;
        doMove[j] = 1; moved++;
    }

    if (!moved) { free(doMove); free(inBody); return 0; }

    /*
     * Punto di inserimento: primo instruction dell'header del loop.
     * L'array risultante sara':
     *   Fase 1: [0, insertAt)              — codice pre-loop invariato
     *   Fase 2: [insertAt, insertAt+moved) — invarianti hoistati (pre-header)
     *   Fase 3: [insertAt+moved, ...)      — header, body, post-loop
     */
    int insertAt = f->blocks[header].start;

    IRInstr *newInstrs = malloc((size_t)(nInstrs + moved) * sizeof(IRInstr));
    int newCount = 0;

    int *map = calloc((size_t)nInstrs, sizeof(int));
    for (int j = 0; j < nInstrs; j++) map[j] = -1;

    /* Fase 1: istruzioni prima dell'header */
    for (int j = 0; j < insertAt; j++) {
        map[j] = newCount;
        newInstrs[newCount++] = f->instrs[j];
    }

    /* Fase 2: istruzioni hoistate -> contenuto del pre-header */
    int phMovedStart = newCount;
    for (int j = 0; j < nInstrs; j++) {
        if (doMove[j]) newInstrs[newCount++] = f->instrs[j];
    }
    int phNewEnd = newCount;

    /* Fase 3: istruzioni da insertAt in poi, saltando quelle mosse */
    for (int j = insertAt; j < nInstrs; j++) {
        if (doMove[j]) continue;
        map[j] = newCount;
        newInstrs[newCount++] = f->instrs[j];
    }

    free(f->instrs);
    f->instrs = newInstrs; f->count = newCount; f->capacity = newCount;

    /* Aggiorna start/end di ogni blocco tramite la mappa */
    for (int b = 0; b < nBlocks; b++) {
        if (b == phIdx) {
            f->blocks[b].start = phMovedStart;
            f->blocks[b].end   = phNewEnd;
            continue;
        }
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
    f->curBlockStart = 0;
    free(doMove); free(inBody); free(map);
    return moved;
}

/* ---- Punto di ingresso ------------------------------------------------- */
int licm_optimize(IRFunction *f) {
    if (!f || f->blockCount == 0 || f->count == 0) return 0;

    int nBlocks = f->blockCount;
    int words   = (nBlocks + 63) / 64;
    Arena *arena = arena_create(0);

    LiveSet *Dom = loop_compute_dominators(f, words, arena);
    Loop *loops  = arena_alloc(arena, MAX_LOOPS * sizeof(Loop));
    int nLoops   = loop_find(f, Dom, loops, arena);
    if (nLoops == 0) { arena_destroy(arena); return 0; }

    Arena *livArena = arena_create(0);
    LivenessResult liv = liveness_compute(f, NULL, livArena);
    int totalMoved = 0;

    for (int l = 0; l < nLoops; l++) {
        Loop *L = &loops[l];
        loop_build_pre_header(f, L);

        VarMap *vm  = &liv.varMap;
        int numVars = liv.numVars;
        int *defCount = countDefsInLoop(f, L, vm, numVars, arena);

        char *invariant = arena_alloc(arena, (size_t)f->count);
        memset(invariant, 0, (size_t)f->count);
        findInvariants(f, L, vm, numVars, defCount, invariant, arena);

        int moved = moveInvariants(f, L, Dom, invariant, defCount, vm, &liv);
        totalMoved += moved;

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