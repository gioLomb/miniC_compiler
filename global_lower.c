#include <stdlib.h>
#include <string.h>
#include "global_lower.h"


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
 * @brief Descriptor for a STORE_ARR that must be emitted right after the
 *        current instruction (scalar global written as dst).
 */
typedef struct {
    Operand addr;     /**< Address temp holding the global's location. */
    Operand val;      /**< Temp holding the value to store. */
    int     present;  /**< 1 if a deferred store is pending, 0 otherwise. */
} DeferredStore;


/**
 * @brief Check whether @p f references any OPND_GLOBAL operand.
 *
 * Cheap fast-path guard: if a function never touches a global variable,
 * gl_lower_globals() can skip the full instruction-array reconstruction
 * entirely.
 *
 * @param f Function to inspect.
 * @return  Non-zero if at least one instruction has an OPND_GLOBAL operand.
 */
static int gl_function_touches_globals(const IRFunction *f) {
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
static void gl_emitter_push(Emitter *e, IROp op, Operand dst,
                          Operand src1, Operand src2, int loopDepth) {
    if (__builtin_expect(e->count == e->cap,0)) {
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
static int gl_emit_global_addr(int symOff,Emitter *e, int loopDepth) {
    int nextTempId = ir_alloc_temp_id();
    gl_emitter_push(e, IR_GLOBAL_ADDR, (Operand){ .kind = OPND_TEMP, .data.tempId = nextTempId }, (Operand){ .kind = OPND_GLOBAL, .data.globalOffset = symOff },
                 (Operand){.kind = OPND_NONE}, loopDepth);
    return nextTempId;
}


/**
 * @brief Lower @p src1 if it is an OPND_GLOBAL, otherwise return it unchanged.
 *
 * Two cases, mirroring the position src1 occupies in the original
 * instruction:
 *   - Array base (src1 of IR_LOAD_ARR): only the address needs to be
 *     materialised; the instruction itself still performs the indexed
 *     access, so no extra LOAD_ARR is emitted here.
 *   - Scalar read (any other opcode): the address is materialised AND
 *     immediately loaded into a fresh temp, since the value itself is
 *     what the instruction needs.
 *
 * @param src1       Original src1 operand.
 * @param isArrBase  1 if src1 is the array base of an IR_LOAD_ARR.
 * @param nextTemp   In/out temp-id counter.
 * @param e          Emitter new instructions are appended to.
 * @param loopDepth  Loop depth stamped on any emitted instruction.
 * @return           Replacement operand for src1 (unchanged if not GLOBAL).
 */
static Operand gl_lower_src1_operand(Operand src1, int isArrBase, Emitter *e, int loopDepth) {
    if (src1.kind != OPND_GLOBAL) return src1;

    int addr = gl_emit_global_addr(src1.data.globalOffset, e, loopDepth);
    if (isArrBase)
        return (Operand){ .kind = OPND_TEMP, .data.tempId = addr };

    int val = ir_alloc_temp_id();
    gl_emitter_push(e, IR_LOAD_ARR,
                 (Operand){ .kind = OPND_TEMP, .isFloat = src1.isFloat, .data.tempId = val },
                 (Operand){ .kind = OPND_TEMP, .data.tempId = addr },
                 (Operand){ .kind = OPND_CONST_INT, .data.intVal = 0 }, loopDepth);
    return (Operand){ .kind = OPND_TEMP, .isFloat = src1.isFloat, .data.tempId = val };
}

/**
 * @brief Lower @p src2 if it is an OPND_GLOBAL, otherwise return it unchanged.
 *
 * src2 is never an array-base position in this IR (that role is always
 * src1 for LOAD_ARR / dst for STORE_ARR), so this is always the scalar
 * read case: address materialisation followed by a LOAD_ARR of index 0.
 *
 * @param src2       Original src2 operand.
 * @param nextTemp   In/out temp-id counter.
 * @param e          Emitter new instructions are appended to.
 * @param loopDepth  Loop depth stamped on any emitted instruction.
 * @return           Replacement operand for src2 (unchanged if not GLOBAL).
 */
static Operand gl_lower_src2_operand(Operand src2, Emitter *e, int loopDepth) {
    if (src2.kind != OPND_GLOBAL) return src2;

    int addr = gl_emit_global_addr(src2.data.globalOffset, e, loopDepth);
    int val  = ir_alloc_temp_id();
    gl_emitter_push(e, IR_LOAD_ARR,
                 (Operand){ .kind = OPND_TEMP, .isFloat = src2.isFloat, .data.tempId = val },
                 (Operand){ .kind = OPND_TEMP, .data.tempId = addr },
                 (Operand){ .kind = OPND_CONST_INT, .data.intVal = 0 }, loopDepth);
    return (Operand){ .kind = OPND_TEMP, .isFloat = src2.isFloat, .data.tempId = val };
}

/**
 * @brief Lower @p dst if it is an OPND_GLOBAL, otherwise return it unchanged.
 *
 * Two cases, mirroring the position dst occupies in the original instruction:
 *   - Array base (dst of IR_STORE_ARR): only the address needs to be
 *     materialised; the instruction itself still performs the store.
 *   - Scalar write (any other defining opcode): the instruction must write
 *     to a fresh temp instead (a global has no local storage slot), and a
 *     STORE_ARR writing that temp back to the global is deferred to
 *     @p out — the caller emits it right after the original instruction.
 *
 * @param dst        Original dst operand.
 * @param isArrBase  1 if dst is the array base of an IR_STORE_ARR.
 * @param nextTemp   In/out temp-id counter.
 * @param e          Emitter new instructions are appended to.
 * @param loopDepth  Loop depth stamped on any emitted instruction.
 * @param out        Set to describe a pending STORE_ARR (out->present = 1)
 *                    when dst was a scalar global; left with present = 0
 *                    otherwise. Caller must zero-init before calling.
 * @return           Replacement operand for dst (unchanged if not GLOBAL).
 */
static Operand gl_lower_dst_operand(Operand dst, int isArrBase,
                                  Emitter *e, int loopDepth, DeferredStore *out) {
    if (dst.kind != OPND_GLOBAL) return dst;

    int addr = gl_emit_global_addr(dst.data.globalOffset, e, loopDepth);
    if (isArrBase)
        return (Operand){ .kind = OPND_TEMP, .data.tempId = addr };

    // scalar write: redirect to a fresh temp now, store it back after the
    // instruction is emitted (handled by the caller)
    int tmpDst = ir_alloc_temp_id();
    out->addr    = (Operand){ .kind = OPND_TEMP, .data.tempId = addr };
    out->val     = (Operand){ .kind = OPND_TEMP, .isFloat = dst.isFloat, .data.tempId = tmpDst };
    out->present = 1;
    return (Operand){ .kind = OPND_TEMP, .isFloat = dst.isFloat, .data.tempId = tmpDst };
}


/**
 * @brief Realign every block's [start, end) range onto the rebuilt
 *        instruction numbering produced by the expansion loop.
 *
 * For each block, its new range spans from the first to the last surviving
 * new-index mapping among its original instructions. A block that becomes
 * empty (no old instruction mapped, e.g. one that only ever held a global
 * store fully absorbed elsewhere — not expected in practice but handled
 * defensively) collapses to an empty range pointing past the rebuilt array.
 *
 * @param f        Function whose f->blocks[] ranges are rewritten in place.
 * @param newStart newStart[oldIdx]: first rebuilt-array index for old
 *                 instruction oldIdx.
 * @param newEnd   newEnd[oldIdx]: one past the last rebuilt-array index
 *                 for old instruction oldIdx.
 * @param newTotal Total instruction count after rebuilding (used as the
 *                 "past the end" sentinel for empty blocks).
 */
static void gl_remap_block_ranges(IRFunction *f, const int *newStart,
                                const int *newEnd, int newTotal) {
    for (int b = 0; b < f->blockCount; b++) {
        int oldS = f->blocks[b].bb.range.start;
        int oldE = f->blocks[b].bb.range.end;
        if (oldE > oldS) {
            f->blocks[b].bb.range.start = newStart[oldS];
            f->blocks[b].bb.range.end   = newEnd[oldE - 1];
        } else {
            // empty block: point past the end
            f->blocks[b].bb.range.start = f->blocks[b].bb.range.end = newTotal;
        }
    }
    f->curBlockStart = 0;
}


void gl_lower_globals(IRFunction *f, Arena *arena) {
    if (f->count == 0 || !gl_function_touches_globals(f)) return;

    int oldCount = f->count;

    // worst case: every original instruction expands into 3 instructions
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

        int isArrBaseSrc1 = (in.op == IR_LOAD_ARR);
        int isArrBaseDst  = (in.op == IR_STORE_ARR);

        in.src1 = gl_lower_src1_operand(in.src1, isArrBaseSrc1, &e, ld);
        in.src2 = gl_lower_src2_operand(in.src2, &e, ld);

        DeferredStore ds = {0};
        in.dst = gl_lower_dst_operand(in.dst, isArrBaseDst, &e, ld, &ds);

        // emit the instruction itself, now with substituted operands
        gl_emitter_push(&e, in.op, in.dst, in.src1, in.src2, ld);

        // emit the deferred store for a scalar global written by this instruction
        if (ds.present)
            gl_emitter_push(&e, IR_STORE_ARR,
                         ds.addr, (Operand){ .kind = OPND_CONST_INT, .data.intVal = 0 }, ds.val, ld);

        newEnd[i] = e.count;
    }

    // replace the function's instruction array with the rebuilt one
    free(f->instrs);
    f->instrs   = e.buf;
    f->count    = e.count;
    f->capacity = e.cap;
    f->ver++;  

    gl_remap_block_ranges(f, newStart, newEnd, e.count);
}