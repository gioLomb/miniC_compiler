/**
 * @file global_lower.c
 * @brief Lowering OPND_GLOBAL -> IR_GLOBAL_ADDR + LOAD/STORE_ARR.
 */

#include <stdlib.h>
#include <string.h>
#include "global_lower.h"


/* =========================================================================
 * Buffer emitter — rebuilds the instruction array one instruction at a time.
 * Kept as a separate structure for clarity with respect to the main loop.
 * ========================================================================= */

/**
 * @brief Growable output buffer used while rebuilding a function's
 *        instruction array during global lowering.
 */
typedef struct {
    IRInstr *buf;   /**< Backing storage, reallocated with doubling growth. */
    int      count; /**< Number of instructions written so far. */
    int      cap;   /**< Current capacity of buf, in instructions. */
} Emitter;


/**
 * @brief Compute the lowest temp id not yet used in @p f.
 *
 * Scans every instruction's dst/src1/src2 operand and returns one past the
 * highest OPND_TEMP id found. This mirrors the pattern already used in
 * sr.c, since the static nextTemp counter owned by ir.c is not exposed to
 * this translation unit and a fresh, function-local counter must be
 * reconstructed here before allocating new temporaries.
 *
 * @param f Function whose instructions are scanned. Not modified.
 * @return  Smallest temp id guaranteed not to collide with an existing one.
 */
static int compute_next_temp(const IRFunction *f) {
    int next = 0;
    for (int i = 0; i < f->count; i++) {
        const Operand *ops[3] = {
            &f->instrs[i].dst, &f->instrs[i].src1, &f->instrs[i].src2
        };
        // check all three operand slots of every instruction
        for (int k = 0; k < 3; k++)
            if (ops[k]->kind == OPND_TEMP && ops[k]->data.tempId >= next)
                next = ops[k]->data.tempId + 1;
    }
    return next;
}

/**
 * @brief Check whether @p f references any OPND_GLOBAL operand.
 *
 * Cheap fast-path guard: if a function never touches a global variable,
 * ir_lower_globals() can skip the full instruction-array reconstruction
 * entirely.
 *
 * @param f Function to inspect.
 * @return  Non-zero if at least one instruction has an OPND_GLOBAL operand.
 */
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

/**
 * @brief Append one instruction to @p e, growing the buffer if needed.
 *
 * @param e         Emitter to append to.
 * @param op        Opcode of the new instruction.
 * @param dst       Destination operand.
 * @param src1      First source operand.
 * @param src2      Second source operand.
 * @param loopDepth Static loop nesting depth to stamp on the instruction,
 *                  inherited from the original instruction being expanded.
 */
static void emitter_push(Emitter *e, IROp op, Operand dst,
                          Operand src1, Operand src2, int loopDepth) {
    if (e->count == e->cap) {
        // standard doubling growth
        e->cap *= 2;
        e->buf  = realloc(e->buf, (size_t)e->cap * sizeof(IRInstr));
    }
    e->buf[e->count++] = (IRInstr){
        .op = op, .dst = dst, .src1 = src1, .src2 = src2, .loopDepth = loopDepth
    };
}

/**
 * @brief Emit a fresh IR_GLOBAL_ADDR instruction materialising the address
 *        of global @p symOff into a new temp.
 *
 * Always emits a brand-new instruction: the address is never cached or
 * reused across instructions (see global_lower.h for the rationale — later
 * passes such as LICM are responsible for hoisting/CSE-ing it if profitable).
 *
 * @param symOff    Symbol offset of the global variable.
 * @param nextTemp  In/out temp-id counter; incremented by one on return.
 * @param e         Emitter the new instruction is appended to.
 * @param loopDepth Loop depth stamped on the new instruction.
 * @return          Temp id holding the computed address.
 */
static int emit_global_addr(int symOff, int *nextTemp,
                             Emitter *e, int loopDepth) {
    int t = (*nextTemp)++;
    emitter_push(e, IR_GLOBAL_ADDR, (Operand){ .kind = OPND_TEMP, .data.tempId = t }, (Operand){ .kind = OPND_GLOBAL, .data.globalOffset = symOff },
                 (Operand){.kind = OPND_NONE}, loopDepth);
    return t;
}


void ir_lower_globals(IRFunction *f, Arena *arena) {
    // nothing to do: empty function, or no OPND_GLOBAL operand anywhere
    if (f->count == 0 || !function_touches_globals(f)) return;

    int oldCount = f->count;
    int nextTemp = compute_next_temp(f);

    // worst case: every original instruction expands into 3 instructions
    // (address computation + load/store + the original instruction itself)
    Emitter e = {
        .buf   = malloc((size_t)(oldCount * IR_MAX_EXPANSION_FACTOR + 8) * sizeof(IRInstr)),
        .count = 0,
        .cap   = oldCount * IR_MAX_EXPANSION_FACTOR + 8
    };

    // maps each old instruction index to its [newStart, newEnd) range in the
    // rebuilt array, so basic-block boundaries can be realigned afterwards
    int *newStart = arena_alloc(arena, (size_t)oldCount * sizeof(int));
    int *newEnd   = arena_alloc(arena, (size_t)oldCount * sizeof(int));

    for (int i = 0; i < oldCount; i++) {
        IRInstr in = f->instrs[i];
        int ld = in.loopDepth;
        newStart[i] = e.count;

        // Distinguishes the two "array base" positions where OPND_GLOBAL is
        // expanded into an address only (the instruction itself performs the
        // load/store):
        //   - src1 of IR_LOAD_ARR  is the base
        //   - dst  of IR_STORE_ARR is the base
        int isArrBaseSrc1 = (in.op == IR_LOAD_ARR);
        int isArrBaseDst  = (in.op == IR_STORE_ARR);

        /* ---- src1 ---- */
        if (in.src1.kind == OPND_GLOBAL) {
            int addr = emit_global_addr(in.src1.data.globalOffset, &nextTemp, &e, ld);
            if (isArrBaseSrc1) {
                // array base: replace with the address temp only, the
                // instruction itself still performs the indexed access
                in.src1 = (Operand){ .kind = OPND_TEMP, .data.tempId = addr };
            } else {
                // scalar read: load the value before it can be used
                int val = nextTemp++;
                emitter_push(&e, IR_LOAD_ARR, (Operand){ .kind = OPND_TEMP, .data.tempId = val }, (Operand){ .kind = OPND_TEMP, .data.tempId = addr }, (Operand){ .kind = OPND_CONST_INT, .data.intVal = 0 }, ld);
                in.src1 = (Operand){ .kind = OPND_TEMP, .data.tempId = val };
            }
        }

        /* ---- src2 (never in an "array base" position) ---- */
        if (in.src2.kind == OPND_GLOBAL) {
            int addr = emit_global_addr(in.src2.data.globalOffset, &nextTemp, &e, ld);
            int val  = nextTemp++;
            emitter_push(&e, IR_LOAD_ARR, (Operand){ .kind = OPND_TEMP, .data.tempId = val }, (Operand){ .kind = OPND_TEMP, .data.tempId = addr }, (Operand){ .kind = OPND_CONST_INT, .data.intVal = 0 }, ld);
            in.src2 = (Operand){ .kind = OPND_TEMP, .data.tempId = val };
        }

        /* ---- dst ----
         * If the destination is a scalar global, redirect the write to a
         * fresh temp and append a STORE_ARR right after the instruction. */
        Operand deferredStoreAddr = (Operand){.kind = OPND_NONE};
        Operand deferredStoreVal  = (Operand){.kind = OPND_NONE};
        int     hasDeferredStore  = 0;

        if (in.dst.kind == OPND_GLOBAL) {
            int addr = emit_global_addr(in.dst.data.globalOffset, &nextTemp, &e, ld);
            if (isArrBaseDst) {
                // array base for STORE_ARR: replace with the address temp
                in.dst = (Operand){ .kind = OPND_TEMP, .data.tempId = addr };
            } else {
                // scalar write: write to a temp now, store it after
                int tmpDst          = nextTemp++;
                deferredStoreAddr   = (Operand){ .kind = OPND_TEMP, .data.tempId = addr };
                deferredStoreVal    = (Operand){ .kind = OPND_TEMP, .data.tempId = tmpDst };
                hasDeferredStore    = 1;
                in.dst              = (Operand){ .kind = OPND_TEMP, .data.tempId = tmpDst };
            }
        }

        // emit the instruction itself, now with substituted operands
        emitter_push(&e, in.op, in.dst, in.src1, in.src2, ld);

        // emit the deferred store for a scalar global written by this instruction
        if (hasDeferredStore)
            emitter_push(&e, IR_STORE_ARR,
                         deferredStoreAddr, (Operand){ .kind = OPND_CONST_INT, .data.intVal = 0 }, deferredStoreVal, ld);

        newEnd[i] = e.count;
    }

    // replace the function's instruction array with the rebuilt one
    free(f->instrs);
    f->instrs   = e.buf;
    f->count    = e.count;
    f->capacity = e.cap;

    // realign every block's [start, end) range onto the new numbering
    for (int b = 0; b < f->blockCount; b++) {
        int oldS = f->blocks[b].bb.range.start;
        int oldE = f->blocks[b].bb.range.end;
        if (oldE > oldS) {
            f->blocks[b].bb.range.start = newStart[oldS];
            f->blocks[b].bb.range.end   = newEnd[oldE - 1];
        } else {
            // empty block: point past the end
            f->blocks[b].bb.range.start = f->blocks[b].bb.range.end = e.count;
        }
    }
    f->curBlockStart = 0;
}