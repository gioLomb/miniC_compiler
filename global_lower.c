/**
 * @file global_lower.c
 * @brief Lowering OPND_GLOBAL -> IR_GLOBAL_ADDR + LOAD/STORE_ARR.
 */

#include <stdlib.h>
#include <string.h>
#include "global_lower.h"

/* =========================================================================
 * Helpers
 * ========================================================================= */

static inline Operand mkT(int id) {
    return (Operand){ .kind = OPND_TEMP, .data.tempId = id };
}
static inline Operand mkI(int v) {
    return (Operand){ .kind = OPND_CONST_INT, .data.intVal = v };
}
static inline Operand mkGlobalOp(int symOff) {
    return (Operand){ .kind = OPND_GLOBAL, .data.globalOffset = symOff };
}

/* Massimo tempId esistente + 1, stesso pattern di sr.c.
 * Non espone il contatore statico nextTemp interno a ir.c. */
static int compute_next_temp(const IRFunction *f) {
    int next = 0;
    for (int i = 0; i < f->count; i++) {
        const Operand *ops[3] = {
            &f->instrs[i].dst, &f->instrs[i].src1, &f->instrs[i].src2
        };
        for (int k = 0; k < 3; k++)
            if (ops[k]->kind == OPND_TEMP && ops[k]->data.tempId >= next)
                next = ops[k]->data.tempId + 1;
    }
    return next;
}

/* Fast-path: evita ricostruzione completa se nessun globale presente. */
static int function_touches_globals(const IRFunction *f) {
    for (int i = 0; i < f->count; i++) {
        const IRInstr *in = &f->instrs[i];
        if (in->dst.kind  == OPND_GLOBAL ||
            in->src1.kind == OPND_GLOBAL ||
            in->src2.kind == OPND_GLOBAL)
            return 1;
    }
    return 0;
}

/* =========================================================================
 * Buffer emitter — ricostruisce istruzione per istruzione il nuovo array.
 * Struttura separata per chiarezza rispetto al loop principale.
 * ========================================================================= */

typedef struct {
    IRInstr *buf;
    int      count;
    int      cap;
} Emitter;

static void emitter_push(Emitter *e, IROp op, Operand dst,
                          Operand src1, Operand src2, int loopDepth) {
    if (e->count == e->cap) {
        e->cap *= 2;
        e->buf  = realloc(e->buf, (size_t)e->cap * sizeof(IRInstr));
    }
    e->buf[e->count++] = (IRInstr){
        .op = op, .dst = dst, .src1 = src1, .src2 = src2, .loopDepth = loopDepth
    };
}

/* Emette IR_GLOBAL_ADDR fresco (mai cachato tra istruzioni: vedi header).
 * Ritorna il tempId del risultato. */
static int emit_global_addr(int symOff, int *nextTemp,
                             Emitter *e, int loopDepth) {
    int t = (*nextTemp)++;
    emitter_push(e, IR_GLOBAL_ADDR, mkT(t), mkGlobalOp(symOff),
                 noOperand(), loopDepth);
    return t;
}

/* =========================================================================
 * Pass principale
 * ========================================================================= */

void ir_lower_globals(IRFunction *f) {
    if (f->count == 0 || !function_touches_globals(f)) return;

    int oldCount = f->count;
    int nextTemp = compute_next_temp(f);

    /* Worst case: ogni istruzione genera 3 istruzioni (addr + load/store + originale). */
    Emitter e = {
        .buf   = malloc((size_t)(oldCount * 3 + 8) * sizeof(IRInstr)),
        .count = 0,
        .cap   = oldCount * 3 + 8
    };

    /* Mappa oldIndex -> [newStart, newEnd) per riallineare i blocchi. */
    int *newStart = malloc((size_t)oldCount * sizeof(int));
    int *newEnd   = malloc((size_t)oldCount * sizeof(int));

    for (int i = 0; i < oldCount; i++) {
        IRInstr in = f->instrs[i];
        int ld = in.loopDepth;
        newStart[i] = e.count;

        /* Distingue le due posizioni "base array" dove OPND_GLOBAL
         * viene espanso in solo indirizzo (l'istruzione stessa fa il load/store):
         *   - src1 di IR_LOAD_ARR  e'  la base
         *   - dst  di IR_STORE_ARR e'  la base          */
        int isArrBaseSrc1 = (in.op == IR_LOAD_ARR);
        int isArrBaseDst  = (in.op == IR_STORE_ARR);

        /* ---- src1 ---- */
        if (in.src1.kind == OPND_GLOBAL) {
            int addr = emit_global_addr(in.src1.data.globalOffset, &nextTemp, &e, ld);
            if (isArrBaseSrc1) {
                /* Array base: rimpiazza solo con il temp indirizzo. */
                in.src1 = mkT(addr);
            } else {
                /* Scalare: carica il valore prima di usarlo. */
                int val = nextTemp++;
                emitter_push(&e, IR_LOAD_ARR, mkT(val), mkT(addr), mkI(0), ld);
                in.src1 = mkT(val);
            }
        }

        /* ---- src2 (mai in posizione "base array") ---- */
        if (in.src2.kind == OPND_GLOBAL) {
            int addr = emit_global_addr(in.src2.data.globalOffset, &nextTemp, &e, ld);
            int val  = nextTemp++;
            emitter_push(&e, IR_LOAD_ARR, mkT(val), mkT(addr), mkI(0), ld);
            in.src2 = mkT(val);
        }

        /* ---- dst ----
         * Se la destinazione e' un globale scalare, ridirige la scrittura
         * su un temp fresco e aggiunge uno STORE_ARR dopo l'istruzione. */
        Operand deferredStoreAddr = noOperand();
        Operand deferredStoreVal  = noOperand();
        int     hasDeferredStore  = 0;

        if (in.dst.kind == OPND_GLOBAL) {
            int addr = emit_global_addr(in.dst.data.globalOffset, &nextTemp, &e, ld);
            if (isArrBaseDst) {
                /* Array base per STORE_ARR: rimpiazza con temp indirizzo. */
                in.dst = mkT(addr);
            } else {
                /* Scalare in scrittura: scrivi su temp, poi store. */
                int tmpDst          = nextTemp++;
                deferredStoreAddr   = mkT(addr);
                deferredStoreVal    = mkT(tmpDst);
                hasDeferredStore    = 1;
                in.dst              = mkT(tmpDst);
            }
        }

        /* Emetti l'istruzione (ora con operandi gia' sostituiti). */
        emitter_push(&e, in.op, in.dst, in.src1, in.src2, ld);

        /* Emetti lo store differito per scalare globale in scrittura. */
        if (hasDeferredStore)
            emitter_push(&e, IR_STORE_ARR,
                         deferredStoreAddr, mkI(0), deferredStoreVal, ld);

        newEnd[i] = e.count;
    }

    /* Sostituisce l'array istruzioni. */
    free(f->instrs);
    f->instrs   = e.buf;
    f->count    = e.count;
    f->capacity = e.cap;

    /* Riallinea [start, end) di ogni blocco sulla nuova numerazione. */
    for (int b = 0; b < f->blockCount; b++) {
        int oldS = f->blocks[b].bb.range.start;
        int oldE = f->blocks[b].bb.range.end;
        if (oldE > oldS) {
            f->blocks[b].bb.range.start = newStart[oldS];
            f->blocks[b].bb.range.end   = newEnd[oldE - 1];
        } else {
            /* Blocco vuoto: punta oltre la fine. */
            f->blocks[b].bb.range.start = f->blocks[b].bb.range.end = e.count;
        }
    }
    f->curBlockStart = 0;

    free(newStart);
    free(newEnd);
}
