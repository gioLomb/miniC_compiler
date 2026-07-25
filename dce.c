#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "dce.h"
#include "hash_table.h"
#include "arena.h"

/* ---- Helper per l'hash di un uint64_t ---- */
static unsigned long uint64_hash(const void *key, size_t keySize) {
    (void)keySize;
    uint64_t v = *(uint64_t *)key;
    v ^= v >> 33;
    v *= 0xff51afd7ed558ccdULL;
    v ^= v >> 33;
    v *= 0xc4ceb9fe1a85ec53ULL;
    v ^= v >> 33;
    return (unsigned long)v;
}

/* ---- Mappa da (tipo, level, offset) a ID compatto ---- */
typedef struct {
    Hash_Table *table;
    int nextId;
} VarMap;

static inline uint64_t make_key(int kind, int level, int offset) {
    uint64_t k = 0;
    k |= (uint64_t)(kind & 0x3) << 62;
    k |= (uint64_t)(level & 0x7fffffff) << 31;
    k |= (uint64_t)(offset & 0x7fffffff);
    return k;
}

// static void varMap_init(VarMap *map) {
//     map->table = ht_create(32, uint64_hash);
//     map->nextId = 0;
// }

// static void varMap_destroy(VarMap *map) {
//     ht_destroy(map->table, NULL);
// }

static int varMap_getOrCreate(VarMap *map, int kind, int level, int offset) {
    uint64_t key = make_key(kind, level, offset);
    int id;
    if (ht_get(map->table, &key, sizeof(key), &id, sizeof(id)))
        return id;
    id = map->nextId++;
    ht_set(map->table, &key, sizeof(key), &id, sizeof(id));
    return id;
}

/* ---- Set di variabili (bitset) ---- */
typedef struct {
    uint64_t *bits;
    int words;
} LiveSet;

static LiveSet liveSet_new(Arena *arena, int words) {
    LiveSet s = {
    .words = words,
    .bits = arena_alloc(arena, (size_t)words * sizeof(uint64_t))};
    memset(s.bits, 0, (size_t)words * sizeof(uint64_t));
    return s;
}

static inline void liveSet_clear(LiveSet *s) {
    memset(s->bits, 0, (size_t)s->words * sizeof(uint64_t));
}

static inline void liveSet_set(LiveSet *s, int id) {
    s->bits[id >> 6] |= 1ULL << (id & 63);
}

static inline void liveSet_clearBit(LiveSet *s, int id) {
    s->bits[id >> 6] &= ~(1ULL << (id & 63));
}

static inline int liveSet_test(const LiveSet *s, int id) {
    return (s->bits[id >> 6] >> (id & 63)) & 1ULL;
}

static inline void liveSet_union(LiveSet *dest, const LiveSet *src) {
    for (int i = 0; i < dest->words; i++)
        dest->bits[i] |= src->bits[i];
}

static inline void liveSet_union_into(LiveSet *dest, const LiveSet *a, const LiveSet *b) {
    for (int i = 0; i < dest->words; i++)
        dest->bits[i] = a->bits[i] | b->bits[i];
}

static inline void liveSet_difference(LiveSet *dest, const LiveSet *a, const LiveSet *b) {
    for (int i = 0; i < dest->words; i++)
        dest->bits[i] = a->bits[i] & ~b->bits[i];
}

static inline int liveSet_equal(const LiveSet *a, const LiveSet *b) {
    for (int i = 0; i < a->words; i++) {
        if (a->bits[i] != b->bits[i]) return 0;
    }
    return 1;
}

static inline void liveSet_copy(LiveSet *dest, const LiveSet *src) {
    memcpy(dest->bits, src->bits, (size_t)src->words * sizeof(uint64_t));
}

/* ---- Purezza ed effetti collaterali ---- */
static inline int isPure(IROp op) {
    switch (op) {
        case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
        case IR_NEG: case IR_NOT:
        case IR_LT: case IR_LE: case IR_GT: case IR_GE: case IR_EQ: case IR_NE:
        case IR_ASSIGN:
        case IR_LOAD_ARR:
            return 1;
        default:
            return 0;
    }
}

static inline int definesDst(IROp op) {
    switch (op) {
        case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
        case IR_NEG: case IR_NOT:
        case IR_LT: case IR_LE: case IR_GT: case IR_GE: case IR_EQ: case IR_NE:
        case IR_ASSIGN:
        case IR_LOAD_ARR:
        case IR_CALL:
            return 1;
        default:
            return 0;
    }
}

static inline int isVarOrTemp(OperandKind kind) {
    return kind == OPND_VAR || kind == OPND_TEMP;
}

static inline int operandId(Operand op, VarMap *map) {
    if (op.kind == OPND_VAR) {
        return varMap_getOrCreate(map, 0, op.data.varLevel, op.data.varOffset);
    } else if (op.kind == OPND_TEMP) {
        return varMap_getOrCreate(map, 1, op.data.tempId, 0);
    }
    return -1;
}

/* ---- Reachability (blocchi raggiungibili) ---- */
static void markReachableBlocks(IRFunction *f, char *reachable) {
    if (f->blockCount == 0) return;
    int *stack = malloc((size_t)f->blockCount * sizeof(int));
    int top = 0;
    stack[top++] = 0;
    reachable[0] = 1;

    while (top > 0) {
        int b = stack[--top];
        for (int k = 0; k < 2; k++) {
            int s = f->blocks[b].succ[k];
            if (s >= 0 && s < f->blockCount && !reachable[s]) {
                reachable[s] = 1;
                stack[top++] = s;
            }
        }
    }
    free(stack);
}

/* ---- DCE ---- */
int dce_optimize(IRFunction *f) {
    if (!f || f->blockCount == 0 || f->count == 0) return 0;

    int nBlocks = f->blockCount;
    int nInstrs = f->count;

    Arena *dce_arena = arena_create(0);

    /* ---- PASSO 0: Reachability ---- */
    char *reachable = arena_alloc(dce_arena, (size_t)nBlocks * sizeof(char));
    memset(reachable, 0, (size_t)nBlocks * sizeof(char));
    markReachableBlocks(f, reachable);

    /* ---- Mappa istruzione -> blocco (O(N) lookup) ---- */
    int *instrToBlock = arena_alloc(dce_arena, (size_t)nInstrs * sizeof(int));
    for (int b = 0; b < nBlocks; b++) {
        for (int i = f->blocks[b].start; i < f->blocks[b].end; i++) {
            instrToBlock[i] = b;
        }
    }

    /* ---- PASSO 1: Raccogli tutte le variabili (assegna ID) ---- */
    VarMap varMap;
    varMap.table = ht_create(32, uint64_hash);
    varMap.nextId = 0;

    for (int i = 0; i < nInstrs; i++) {
        IRInstr *in = &f->instrs[i];
        int blockIdx = instrToBlock[i];
        if (!reachable[blockIdx]) continue;

        if (definesDst(in->op) && isVarOrTemp(in->dst.kind))
            operandId(in->dst, &varMap);
        if (isVarOrTemp(in->src1.kind))
            operandId(in->src1, &varMap);
        if (isVarOrTemp(in->src2.kind))
            operandId(in->src2, &varMap);
    }

    int numVars = varMap.nextId;
    int words = (numVars + 63) / 64;

    /* ---- PASSO 2: Alloca Use, Def, LiveIn, LiveOut per ogni blocco ---- */
    LiveSet *Use    = arena_alloc(dce_arena, (size_t)nBlocks * sizeof(LiveSet));
    LiveSet *Def    = arena_alloc(dce_arena, (size_t)nBlocks * sizeof(LiveSet));
    LiveSet *LiveIn  = arena_alloc(dce_arena, (size_t)nBlocks * sizeof(LiveSet));
    LiveSet *LiveOut = arena_alloc(dce_arena, (size_t)nBlocks * sizeof(LiveSet));
    for (int b = 0; b < nBlocks; b++) {
        Use[b]    = liveSet_new(dce_arena, words);
        Def[b]    = liveSet_new(dce_arena, words);
        LiveIn[b]  = liveSet_new(dce_arena, words);
        LiveOut[b] = liveSet_new(dce_arena, words);
    }

    /* ---- PASSO 3: Calcola Use e Def solo per blocchi raggiungibili ---- */
    for (int b = 0; b < nBlocks; b++) {
        if (!reachable[b]) continue;
        int start = f->blocks[b].start;
        int end = f->blocks[b].end;
        for (int i = start; i < end; i++) {
            IRInstr *in = &f->instrs[i];
            if (definesDst(in->op) && isVarOrTemp(in->dst.kind)) {
                int id = operandId(in->dst, &varMap);
                liveSet_set(&Def[b], id);
            }
            if (isVarOrTemp(in->src1.kind)) {
                int id = operandId(in->src1, &varMap);
                if (!liveSet_test(&Def[b], id))
                    liveSet_set(&Use[b], id);
            }
            if (isVarOrTemp(in->src2.kind)) {
                int id = operandId(in->src2, &varMap);
                if (!liveSet_test(&Def[b], id))
                    liveSet_set(&Use[b], id);
            }
        }
    }

    /* ---- PASSO 4: Liveness dataflow (backward), solo blocchi raggiungibili ---- */
    LiveSet tmp = liveSet_new(dce_arena, words);
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int b = nBlocks - 1; b >= 0; b--) {
            if (!reachable[b]) continue;
            /* LiveOut = union LiveIn[succ] (solo per successori raggiungibili) */
            liveSet_clear(&LiveOut[b]);
            for (int k = 0; k < 2; k++) {
                int s = f->blocks[b].succ[k];
                if (s >= 0 && s < nBlocks && reachable[s]) {
                    liveSet_union(&LiveOut[b], &LiveIn[s]);
                }
            }

            /* LiveIn = Use ∪ (LiveOut - Def) */
            liveSet_difference(&tmp, &LiveOut[b], &Def[b]);
            liveSet_union_into(&tmp, &tmp, &Use[b]);

            if (!liveSet_equal(&LiveIn[b], &tmp)) {
                liveSet_copy(&LiveIn[b], &tmp);
                changed = 1;
            }
        }
    }

    /* ---- PASSO 5: Mark (backward dentro ogni blocco raggiungibile) ---- */
    char *eliminate = arena_alloc(dce_arena, (size_t)nInstrs * sizeof(char));
    memset(eliminate, 0, (size_t)nInstrs * sizeof(char));
    for (int b = 0; b < nBlocks; b++) {
        if (!reachable[b]) {
            for (int i = f->blocks[b].start; i < f->blocks[b].end; i++) {
                eliminate[i] = 1;
            }
            continue;
        }

        LiveSet Live = liveSet_new(dce_arena, words);
        liveSet_copy(&Live, &LiveOut[b]);

        int start = f->blocks[b].start;
        int end = f->blocks[b].end;
        for (int i = end - 1; i >= start; i--) {
            IRInstr *in = &f->instrs[i];
            int def = definesDst(in->op) && isVarOrTemp(in->dst.kind);
            int dstId = def ? operandId(in->dst, &varMap) : -1;

            if (isPure(in->op) && def && !liveSet_test(&Live, dstId)) {
                eliminate[i] = 1;
                continue;
            }

            if (isVarOrTemp(in->src1.kind)) {
                int id = operandId(in->src1, &varMap);
                liveSet_set(&Live, id);
            }
            if (isVarOrTemp(in->src2.kind)) {
                int id = operandId(in->src2, &varMap);
                liveSet_set(&Live, id);
            }
            if (def && dstId >= 0) {
                liveSet_clearBit(&Live, dstId);
            }
        }
    }

    /* ---- PASSO 6: Sweep (compatta le istruzioni rimanenti) ---- */
    int *map = arena_alloc(dce_arena, (size_t)nInstrs * sizeof(int));
    for (int i = 0; i < nInstrs; i++) map[i] = -1;

    IRInstr *newInstrs = malloc((size_t)nInstrs * sizeof(IRInstr));
    int newCount = 0;
    for (int i = 0; i < nInstrs; i++) {
        if (!eliminate[i]) {
            newInstrs[newCount] = f->instrs[i];
            map[i] = newCount;
            newCount++;
        }
    }

    free(f->instrs);
    f->instrs = newInstrs;
    f->count = newCount;
    f->capacity = newCount;  /* aggiorniamo la capacità per coerenza */

    /* Aggiorna start/end dei blocchi */
    for (int b = 0; b < nBlocks; b++) {
        if (f->blocks[b].start == f->blocks[b].end) {
            f->blocks[b].start = f->blocks[b].end = 0;
            continue;
        }
        int oldStart = f->blocks[b].start;
        int oldEnd = f->blocks[b].end;
        int newStart = -1, newEnd = -1;
        for (int i = oldStart; i < oldEnd; i++) {
            if (map[i] != -1) {
                if (newStart == -1) newStart = map[i];
                newEnd = map[i] + 1;
            }
        }
        if (newStart == -1) {
            newStart = newEnd = 0;
        }
        f->blocks[b].start = newStart;
        f->blocks[b].end = newEnd;
    }
    f->curBlockStart = 0;

    /* ---- PASSO 7: Pulizia ---- */
    arena_destroy(dce_arena);
    ht_destroy(varMap.table, NULL);
    return newCount != nInstrs;
}