#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "dce.h"
#include "liveness.h"
#include "arena.h"


/**
 * @brief Mark every basic block reachable from the entry block (index 0).
 *
 * Uses an explicit stack-based DFS to avoid call-stack overflow on functions
 * with deeply nested control flow.  @p reachable must be zero-initialised by
 * the caller; on return, reachable[b] is 1 for every block reachable from
 * block 0 via succ[] edges.
 *
 * @param f          IR function whose CFG is traversed.
 * @param reachable  Output array of length @c f->blockCount (caller-zeroed).
 */
static void dce_mark_reachable_blocks(IRFunction *f, char *reachable, Arena *arena) {
    if (f->blockCount == 0) return;

    // worst case: every block on the stack once → blockCount entries suffice
    int *stack = arena_alloc(arena, (size_t)f->blockCount * sizeof(int));
    int top = 0;

    // seed the DFS from the function entry block
    stack[top++] = 0;
    reachable[0] = 1; // mark before push so we never push the same block twice

    while (top > 0) {
        int b = stack[--top];
        for (int k = 0; k < 2; k++) { // each block has at most 2 successors (succ[0], succ[1])
            int s = f->blocks[b].bb.succ[k];
            // bounds check guards against sentinel -1 and stale succ values
            if (s >= 0 && s < f->blockCount && !reachable[s]) {
                reachable[s] = 1; // mark before push: prevents re-enqueuing in cyclic CFGs
                stack[top++] = s;
            }
        }
    }
}

/* Mark: backward liveness scan */
/**
 * @brief Mark @p op's bit as live if it is a storage operand.
 *
 * Constants, labels, and function names carry no VarMap id and are
 * silently skipped.
 */
static inline void dce_mark_operand_live(BitSet *live, Operand op, VarMap *vm) {
    if (!ir_operand_is_storage(op.kind)) return;
    int id = varmap_operand_id(vm, op);
    if (id >= 0) bitset_set(live, id);
}

/**
 * @brief Mark every instruction in block @p b as dead (unreachable block).
 */
static inline void dce_mark_block_dead(IRFunction *f, int b, char *eliminate) {
    for (int i = f->blocks[b].bb.range.start; i < f->blocks[b].bb.range.end; i++)
        eliminate[i] = 1;
}


static int dce_process_instr(IRInstr *in, int idx, BitSet *live, VarMap *vm, char *eliminate) {
    int dstId = varmap_operand_id(vm, in->dst);
    int def   = ir_defines_dst(in->op) && dstId >= 0;

    if ((ir_is_pure(in->op) || in->op == IR_LOAD_ARR) && def && !bitset_test(live, dstId)) {
        eliminate[idx] = 1;
        return 1;
    }

    // Backward transfer: LiveIn = Use ∪ (LiveOut − Def). Def MUST be cleared
    // before uses are added, otherwise a self-referential update (dst == src,
    // e.g. "x = x + y") loses its own use when def-clear wipes the bit just set.
    if (def)
        bitset_clr(live, dstId);

    int s1 = varmap_operand_id(vm, in->src1);
    int s2 = varmap_operand_id(vm, in->src2);
    if (s1 >= 0) bitset_set(live, s1);
    if (s2 >= 0) bitset_set(live, s2);

    if (!ir_defines_dst(in->op) && dstId >= 0)
        bitset_set(live, dstId);

    return 0;
}

static void dce_mark(IRFunction *f, const char *reachable, LivenessResult *liv,
                      Arena *arena, char *eliminate) {
    int nBlocks = f->blockCount;
    int words   = liv->blockSets.words;
    memset(eliminate, 0, (size_t)f->count);

    for (int b = 0; b < nBlocks; b++) {
        if (!reachable[b]) {
            dce_mark_block_dead(f, b, eliminate);
            continue;
        }

        BitSet live = bitset_new(arena, words);
        bitset_copy(&live, &liv->blockSets.LiveOut[b]);

        // reverse scan: liveness is a backward dataflow
        for (int i = f->blocks[b].bb.range.end - 1; i >= f->blocks[b].bb.range.start; i--)
            dce_process_instr(&f->instrs[i], i, &live, liv->varMap, eliminate);
    }
}

int dce_optimize(IRFunction *f, VarMap *vm, Arena *arenaScratch) {
    if (!f || f->blockCount == 0 || f->count == 0) return 0;

    int nBlocks = f->blockCount;
    arena_reset(arenaScratch);
    char *reachable = arena_alloc(arenaScratch, (size_t)nBlocks);
    memset(reachable, 0, (size_t)nBlocks);
    dce_mark_reachable_blocks(f, reachable, arenaScratch);

    LivenessResult liv = liveness_computeIr(f, reachable, vm, arenaScratch);

    char *eliminate = arena_alloc(arenaScratch, (size_t)f->count);
    dce_mark(f, reachable, &liv, arenaScratch, eliminate);

    int changed = ir_sweep(f, eliminate, nBlocks); // compact + fix block ranges

    return changed;
}