#include <stdlib.h>
#include <string.h>
#include "licm.h"
#include "loop.h"
#include "liveness.h"
#include "arena.h"
#include "dynamic_array.h"


/* =========================================================================
 * Phase 1 — Def-count inside the loop
 * =========================================================================
 * Walk every instruction in every block of the loop body and count how
 * many times each tracked operand (variable or temporary) is defined.
 * The counts guide invariant detection:
 *   defCount == 0  → defined outside the loop, trivially invariant
 *   defCount == 1  → single definition, may be invariant (checked in phase 2)
 *   defCount >= 2  → redefined, can never be invariant
 * ========================================================================= */
/**
 * @brief Count definitions of every tracked operand inside loop @p L, and
 *        record the instruction index of each single-definition variable's
 *        unique definition site.
 *
 * @p outDefInstrIdx receives an array of length @p numVars, initialised to
 * -1, set to the defining instruction's index whenever a definition is seen.
 * For ids with defCount == 1 this is guaranteed to be THE unique definition;
 * for ids with defCount >= 2 the value is meaningless (last write wins) and
 * callers must never read it without checking defCount == 1 first.
 *
 * This turns the repeated "find where variable X is defined in this loop"
 * scan (previously redone on every worklist pop inside propagate_worklist ->
 * src_is_invariant -> licm_single_loop_def_invariant, up to O(loop size) per call)
 * into a single O(loop size) pass done once here.
 */
static int *count_defs_in_loop(IRFunction *f, Loop *L, VarMap *vm,
                               int numVars, Arena *arena, int **outDefInstrIdx) {
    int *defCount = arena_alloc(arena, (size_t)numVars * sizeof(int));
    memset(defCount, 0, (size_t)numVars * sizeof(int));

    int *defInstrIdx = arena_alloc(arena, (size_t)numVars * sizeof(int));
    memset(defInstrIdx, -1, (size_t)numVars * sizeof(int)); // -1 = no def seen yet

    const int bodyCount = L->bodyCount;
    const IRBlock *blocks = f->blocks;
    const IRInstr *instrs = f->instrs;

    for (int i = 0; i < bodyCount; i++) {
        int b = L->body[i];

        for (int j = blocks[b].bb.range.start; j < blocks[b].bb.range.end; j++) {
            const IRInstr *in = &instrs[j];
            if (ir_defines_dst(in->op) && ir_operand_is_storage(in->dst.kind)) {
                int id = varmap_operand_id(vm, in->dst);
                if (id >= 0) {
                    defCount[id]++;
                    defInstrIdx[id] = j; // meaningful only when defCount[id] == 1
                }
            }
        }
    }

    *outDefInstrIdx = defInstrIdx;
    return defCount;
}


/**
 * @brief O(1) check whether @p id's unique in-loop definition is invariant.
 *
 * Only valid when the caller already knows defCount[id] == 1. The defining
 * instruction's index is static (precomputed once by count_defs_in_loop());
 * only its invariance flag is dynamic (advances as propagate_worklist() runs).
 */
static inline int licm_single_loop_def_invariant(const int *defInstrIdx,
                                            const char *invariant, int id) {
    int idx = defInstrIdx[id];
    return (idx >= 0 && invariant[idx]) ? idx : -1;
}

static int src_is_invariant(Operand src, const int *defCount,
                           const int *defInstrIdx, VarMap *vm, const char *invariant) {
   if (src.kind == OPND_CONST_INT || src.kind == OPND_CONST_FLOAT ||
        src.kind == OPND_NONE      || src.kind == OPND_GLOBAL) return 1;

    if (!ir_operand_is_storage(src.kind)) return 0;

    int id = varmap_operand_id(vm, src);

    if (id < 0 || defCount[id] == 0) return 1;

    if (defCount[id] == 1) return licm_single_loop_def_invariant(defInstrIdx, invariant, id) >= 0;

    return 0;
}


/** Historical name preserved: UsedByList is now just an IntVector. */
typedef IntVector UsedByList;

/**
 * @brief Free the dynamic arrays of all @p numVars UsedByList entries.
 */
static void free_used_by(UsedByList *usedBy, int numVars) {
    for (int i = 0; i < numVars; i++)
        int_vector_free(&usedBy[i]);
}

/**
 * @brief Allocate and zero-initialize a usedBy[] array of length numVars.
 *
 * usedBy[id] = list of instruction indices (in the loop) that read operand id.
 */
static UsedByList *alloc_used_by(int numVars) {
    UsedByList *usedBy = calloc((size_t)numVars, sizeof(UsedByList));
    if (!usedBy) abort();
    for (int id = 0; id < numVars; id++) int_vector_init(&usedBy[id], 0);
    return usedBy;
}

/**
 * @brief Register instruction j as a user of both its source operands.
 *
 * For each source operand that resolves to a tracked var id, push
 * instruction index j into usedBy[id], so propagate_worklist can later
 * find and re-check this instruction once that id becomes invariant.
 *
 * @param vm      VarMap for id resolution.
 * @param usedBy  Reverse-use map to update.
 * @param in      Instruction being registered.
 * @param j       Index of in inside f->instrs.
 */
static void register_src_uses(VarMap *vm, UsedByList *usedBy, IRInstr *in, int j) {
    Operand srcs[2] = { in->src1, in->src2 };
    for (int s = 0; s < 2; s++) {
        int id = varmap_operand_id(vm, srcs[s]);
        if (id >= 0) int_vector_push(&usedBy[id], j);
    }
}

/**
 * @brief Scan the loop body for pure, storage-defining instructions:
 *        register each as a user of its source operands in usedBy[], and
 *        enqueue it into worklist as a seed candidate (its sources may
 *        already be invariant).
 *
 * @param f        IR function.
 * @param L        Loop being analysed.
 * @param vm       VarMap for id resolution.
 * @param usedBy   Reverse-use map to populate (must already be allocated).
 * @param worklist Output worklist, sized f->count by the caller.
 * @return Number of instructions seeded into worklist (the new wTail).
 */
static int licm_seed_worklist(IRFunction *f, Loop *L, VarMap *vm,
                         UsedByList *usedBy, int *worklist) {
    int wTail = 0;

    const int bodyCount = L->bodyCount;
    const IRBlock *blocks = f->blocks;
    const IRInstr *instrs = f->instrs;

    for (int i = 0; i < bodyCount; i++) {
        int b = L->body[i];

        for (int j = blocks[b].bb.range.start; j < blocks[b].bb.range.end; j++) {
            const IRInstr *in = &instrs[j];

            // Filter pure candidate instructions writing to a storage destination
            if (ir_is_pure(in->op) && ir_defines_dst(in->op) &&
                ir_operand_is_storage(in->dst.kind)) {

                // register this instruction as a user of its source operands
                register_src_uses(vm, usedBy, (IRInstr *)in, j);

                // immediately enqueue: sources may already be invariant
                worklist[wTail++] = j;
            }
        }
    }

    return wTail;
}
/**
 * @brief Push into worklist every not-yet-invariant instruction that uses
 *        dstId, so it gets re-checked now that dstId is invariant.
 *
 * No-op if dstId is negative (operand not tracked).
 *
 * @param usedBy    Reverse-use map.
 * @param dstId     Var id that just became invariant (-1 if none).
 * @param worklist  Worklist array, grown in place.
 * @param wTail     In/out cursor: current worklist length, advanced by
 *                  the number of dependents pushed.
 * @param invariant invariant[] flags, used to skip already-marked instrs.
 */
static void enqueue_dependents(const UsedByList *usedBy, int dstId,
                                int *worklist, int *wTail, const char *invariant) {
    if (dstId < 0) return;
    for (int k = 0; k < usedBy[dstId].len; k++) {
        int dep = usedBy[dstId].data[k];
        if (!invariant[dep])
            worklist[(*wTail)++] = dep;
    }
}


static void propagate_worklist(IRFunction *f, const int *defCount, const int *defInstrIdx,
                                VarMap *vm, UsedByList *usedBy,
                                int *worklist, int wTail, char *invariant) {
    int wHead = 0;

    while (wHead < wTail) {
        int j = worklist[wHead++];
        IRInstr *in = &f->instrs[j];

        if (!src_is_invariant(in->src1, defCount, defInstrIdx, vm, invariant)) continue;
        if (!src_is_invariant(in->src2, defCount, defInstrIdx, vm, invariant)) continue;

        if (invariant[j]) continue;

        invariant[j] = 1;

        int dstId = varmap_operand_id(vm, in->dst);
        enqueue_dependents(usedBy, dstId, worklist, &wTail, invariant);
    }
}


static void find_invariants(IRFunction *f, Loop *L, VarMap *vm,
                            int numVars, const int *defCount, const int *defInstrIdx,
                            char *invariant, Arena *arena) {
    int n = f->count;
    UsedByList *usedBy = alloc_used_by(numVars);
    int *worklist = arena_alloc(arena, (size_t)n * sizeof(int));

    int wTail = licm_seed_worklist(f, L, vm, usedBy, worklist);
    propagate_worklist(f, defCount, defInstrIdx, vm, usedBy, worklist, wTail, invariant);

    free_used_by(usedBy, numVars);
    free(usedBy);
}


/**
 * @brief Return non-zero if block @p blk dominates all exit blocks of @p L.
 *
 * If the block containing an invariant instruction does not dominate every
 * loop exit, hoisting would cause the instruction to execute on iterations
 * that would have exited before reaching it — a change in semantics
 * (and potentially a crash if the instruction would have trapped).
 *
 * @param L    Loop whose exit blocks are checked.
 * @param Dom  Dominator sets from loop_compute_dominators.
 * @param blk  Block index to test.
 * @return     1 if @p blk dominates every exit block of @p L.
 */
static inline int dominates_all_exits(Loop *L, LiveSet *Dom, int blk) {
    for (int i = 0; i < L->exitCount; i++)
        if (!loop_dominates(Dom, blk, L->exits[i])) return 0;
    return 1;
}


/**
 * @brief Build a flat instruction-index -> block-index lookup table.
 *
 * Blocks partition f->instrs[] contiguously in increasing order (see
 * ir_close_block() in ir.c: every block's [start,end) is disjoint and blocks
 * appear in index order), so a single forward scan over blocks fills the
 * whole map in O(instrs + blocks) total. Replaces the previous pattern of
 * calling instr_block() (O(blocks) linear scan) once per instruction inside
 * licm_mark_hoistable()'s loop over every instruction in the function — an
 * O(instrs * blocks) cost per loop processed by LICM.
 *
 * @param f      IR function whose blocks are scanned.
 * @param arena  Arena the output array is allocated from.
 * @return       Arena-allocated array of length f->count; instrToBlock[j] is
 *               the index of the block containing instruction j.
 */
static int *build_instr_to_block(IRFunction *f, Arena *arena) {
    int *instrToBlock = arena_alloc(arena, (size_t)f->count * sizeof(int));
    for (int b = 0; b < f->blockCount; b++)
        for (int j = f->blocks[b].bb.range.start; j < f->blocks[b].bb.range.end; j++)
            instrToBlock[j] = b;
    return instrToBlock;
}


/**
 * @brief True if every in-loop source of instruction @p j is either
 *        loop-invariant outside, or itself selected for hoisting (@p doMove).
 *
 * Prevents hoisting t1=x+1 when x=5 was marked invariant but not hoisted
 * (e.g. because x is LiveIn at the header): otherwise the preheader would
 * read a stale x.
 */
static int licm_sources_ok_to_hoist(IRFunction *f, int j, const int *defCount,
                                   const int *defInstrIdx, VarMap *vm,
                                   const char *doMove) {
    const IRInstr *in = &f->instrs[j];
    Operand srcs[2] = { in->src1, in->src2 };
    for (int s = 0; s < 2; s++) {
        if (!ir_operand_is_storage(srcs[s].kind)) continue;
        int id = varmap_operand_id(vm, srcs[s]);
        if (id < 0 || defCount[id] == 0) continue; /* outside loop or none */
        if (defCount[id] != 1) return 0;
        int defJ = defInstrIdx[id];
        if (defJ < 0 || !doMove[defJ]) return 0;
    }
    return 1;
}

static int licm_mark_hoistable(IRFunction *f, Loop *L, LiveSet *Dom,
                           const char *invariant, const int *defCount,
                           const int *defInstrIdx, VarMap *vm,
                           LivenessResult *liv, int header,
                           const char *inBody, const int *instrToBlock, char *doMove) {
    int moved = 0;

    for (int j = 0; j < f->count; j++) {
        if (!invariant[j]) continue;

        int blk = instrToBlock[j];  // O(1) lookup instead of instr_block() scan
        if (!inBody[blk]) continue;

        // containing block must dominate all loop exits
        if (!dominates_all_exits(L, Dom, blk)) continue;

        // exactly one definition of the destination in the loop
        int dstId = varmap_operand_id(vm, f->instrs[j].dst);
        if (dstId < 0 || defCount[dstId] != 1) continue;

        // destination must not be live-in at the header
        if (bitset_test(&liv->blockSets.LiveIn[header], dstId)) continue;

        doMove[j] = 1;
        moved++;
    }

    /* Drop hoists whose in-loop sources are not themselves hoisted.  Iterate
     * to a fixed point so chains collapse correctly. */
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int j = 0; j < f->count; j++) {
            if (!doMove[j]) continue;
            if (licm_sources_ok_to_hoist(f, j, defCount, defInstrIdx, vm, doMove))
                continue;
            doMove[j] = 0;
            moved--;
            changed = 1;
        }
    }

    return moved;
}


/** @brief Result of compacting instrs into [prefix | hoisted | rest] layout. */
typedef struct {
    IRInstr *instrs;       // newly allocated, compacted instruction array
    int      count;        // number of instructions in instrs
    int      preHeaderMovedStart; // first index of the hoisted block (new pre-header body)
    int      preHeaderMovedEnd;   // one-past-last index of the hoisted block
} HoistLayout;

/**
 * @brief Rebuild the instruction array with hoisted instructions moved
 *        right before the loop header, and fill oldToNew for remapping.
 *
 * Layout of the new array: [0, insertAt) unchanged prefix, then every
 * instruction with doMove[j] set (this becomes the pre-header body), then
 * every remaining instruction from [insertAt, nInstrs) in original order.
 *
 * @param f         IR function (source instrs, not yet modified).
 * @param doMove    doMove[j] = 1 if instruction j must be hoisted.
 * @param moved     Number of instructions with doMove[j] set (== count of 1s).
 * @param insertAt  First instruction index of the loop header block.
 * @param oldToNew  Output map, length f->count. Filled for EVERY j with
 *                  doMove[j] == 0 (both the unchanged prefix and the
 *                  surviving suffix); left untouched for hoisted j — callers
 *                  must gate access on doMove[j], not on a sentinel value.
 * @return Layout describing the new array and where the hoisted block sits.
 */ 
static HoistLayout licm_compact_and_hoist(IRFunction *f, const char *doMove, int moved,
                                      int insertAt, int *oldToNew) {
    int nInstrs = f->count;
    HoistLayout out;
    out.instrs = malloc((size_t)(nInstrs + moved) * sizeof(IRInstr));
    out.count  = 0;

    //instructions before the loop header
    for (int j = 0; j < insertAt; j++) {
        oldToNew[j] = j;
        out.instrs[out.count++] = f->instrs[j];
    }

    //  hoisted instructions that will form the pre-header body 
    out.preHeaderMovedStart = out.count;
    for (int j = 0; j < nInstrs; j++) {
        if (doMove[j]) out.instrs[out.count++] = f->instrs[j];
    }
    out.preHeaderMovedEnd = out.count;

    // remaining loop instructions (skipping moved ones) ---
    for (int j = insertAt; j < nInstrs; j++) {
        if (doMove[j]) continue; // already placed in part b
        oldToNew[j] = out.count;
        out.instrs[out.count++] = f->instrs[j];
    }

    return out;
}

/**
 * @brief Rewrite block.start/end ranges to match the compacted instruction
 *        array produced by licm_compact_and_hoist.
 *
 * The pre-header block is special-cased: it now contains exactly the
 * hoisted instructions ([preHeaderMovedStart, preHeaderMovedEnd)). Every other block's
 * range is derived by mapping its old instructions through oldToNew and
 * skipping the ones that were hoisted out of it; a block left with no
 * surviving instructions becomes the empty range {0, 0}.
 *
 * @param f            IR function whose blocks[] are rewritten in place.
 * @param oldToNew     Map from licm_compact_and_hoist, valid for every j with
 *                      doMove[j] == 0.
 * @param doMove       doMove[j] = 1 if instruction j was hoisted.
 * @param phIdx        Index of the pre-header block.
 * @param preHeaderMovedStart First index of the hoisted block in the new array.
 * @param preHeaderMovedEnd   One-past-last index of the hoisted block.
 */
static void licm_remap_block_ranges(IRFunction *f, const int *oldToNew, const char *doMove,
                                int phIdx, int preHeaderMovedStart, int preHeaderMovedEnd) {
    for (int b = 0; b < f->blockCount; b++) {
        if (b == phIdx) {
            // pre-header now contains exactly the hoisted instructions
            f->blocks[b].bb.range.start = preHeaderMovedStart;
            f->blocks[b].bb.range.end   = preHeaderMovedEnd;
            continue;
        }

        int oldS = f->blocks[b].bb.range.start, oldE = f->blocks[b].bb.range.end;
        int newS = -1, newE = -1;

        for (int j = oldS; j < oldE; j++) {

            if (doMove[j]) continue; // this instruction was hoisted out of b
            if (newS == -1) newS = oldToNew[j];
            newE = oldToNew[j] + 1;
        }

        f->blocks[b].bb.range.start = (newS == -1) ? oldS : newS;
        f->blocks[b].bb.range.end   = (newE == -1) ? oldS : newE;
    }
}

/**
 * @brief Move safe loop-invariant instructions from the loop body to the
 *        pre-header.
 *
 * Safety conditions (both required):
 *   1. The containing block dominates all loop exits (dominates_all_exits).
 *   2. The destination has exactly one definition in the loop AND is not
 *      live-in at the loop header (moving it would not create a new value
 *      visible on paths that bypass the pre-header).
 *
 * After motion, block start/end indices for all blocks are updated to
 * reflect the new layout.  f->curBlockStart is reset to 0.
 *
 * @param f         IR function (modified in place).
 * @param L         Loop whose invariants are to be hoisted.
 * @param Dom       Dominator sets.
 * @param invariant Per-instruction invariance flags from find_invariants.
 * @param defCount  Per-id definition counts from count_defs_in_loop.
 * @param vm        VarMap for id resolution.
 * @param liv       Liveness result used to query live-in at the header.
 * @return          Number of instructions actually moved (0 = nothing changed).
 */
static int licm_move_invariants(IRFunction *f, Loop *L, LiveSet *Dom,
                           const char *invariant, const int *defCount,
                           const int *defInstrIdx, VarMap *vm,
                           LivenessResult *liv) {
    int nInstrs = f->count, nBlocks = f->blockCount;
    int header  = L->header, phIdx = L->preHeader;

    Arena *localArena = arena_create(0);

    char *doMove = arena_alloc(localArena, (size_t)nInstrs);
    char *inBody = arena_alloc(localArena, (size_t)nBlocks);
    memset(doMove, 0, (size_t)nInstrs);
    memset(inBody, 0, (size_t)nBlocks);

    for (int i = 0; i < L->bodyCount; i++) inBody[L->body[i]] = 1;

    // precomputed once per loop instead of per-instruction linear scan
    int *instrToBlock = build_instr_to_block(f, localArena);

    int moved = licm_mark_hoistable(f, L, Dom, invariant, defCount, defInstrIdx,
                                vm, liv, header, inBody, instrToBlock, doMove);
    if (!moved) { arena_destroy(localArena); return 0; }

    // hoisted instructions are placed in the pre-header just before it
    int insertAt = f->blocks[header].bb.range.start;

    int *oldToNew = arena_alloc(localArena, (size_t)nInstrs * sizeof(int));

    HoistLayout layout = licm_compact_and_hoist(f, doMove, moved, insertAt, oldToNew);

    free(f->instrs);
    f->instrs   = layout.instrs;
    f->count    = layout.count;
    f->capacity = layout.count;

    licm_remap_block_ranges(f, oldToNew, doMove, phIdx, layout.preHeaderMovedStart, layout.preHeaderMovedEnd);

    f->curBlockStart = 0;
    arena_destroy(localArena);
    return moved;
}


int licm_optimize(IRFunction *f, Arena *arenaScratch) {
    if (!f || f->blockCount == 0 || f->count == 0) return 0;

    int nBlocks = f->blockCount;
    int words   = (nBlocks + 63) / 64;
    // Outer scratch (dominators, loop descriptors, invariant flags): caller-owned.
    arena_reset(arenaScratch);

    // compute dominators and find natural loops
    LiveSet *Dom    = loop_compute_dominators(f, words, arenaScratch);
    Loop    *loops  = arena_alloc(arenaScratch, MAX_LOOPS * sizeof(Loop));
    int      nLoops = loop_find(f, Dom, loops, arenaScratch);
    if (nLoops == 0) return 0;


    Arena         *livArena = arena_create(0);
    LivenessResult  liv     = liveness_computeIr(f, NULL, NULL, livArena);
    int totalMoved = 0;

    for (int l = 0; l < nLoops; l++) {
        Loop *L = &loops[l];

        // insert the synthetic pre-header; returns 0 iff entry↔preheader swap
        int phRet = loop_build_pre_header(f, L, loops, nLoops);

        /*
         * Only the entry-header swap invalidates Dom/liv:
         *   - block indices are remapped (0 ↔ phIdx)
         *   - Dom is too short and would OOB on the new header index
         *   - LiveIn[] is indexed by the post-swap header
         *
         * Without swap the preheader is only appended: body/exit indices and
         * Dom[0..oldN) stay valid (hoist queries never touch the new block),
         * and LiveIn[header] still refers to the same block index.  Matching
         * the pre-fix behaviour for that common case avoids O(L) full rebuilds.
         */
        if (phRet == 0) {
            nBlocks = f->blockCount;
            words   = (nBlocks + 63) / 64;
            Dom     = loop_compute_dominators(f, words, arenaScratch);

            varmap_destroy(liv.varMap);
            arena_destroy(livArena);
            livArena = arena_create(0);
            liv      = liveness_computeIr(f, NULL, NULL, livArena);
        }

        VarMap *vm  = liv.varMap;
        int numVars = liv.blockSets.numVars;

        int *defInstrIdx;
        int *defCount = count_defs_in_loop(f, L, vm, numVars, arenaScratch, &defInstrIdx);

        char *invariant = arena_alloc(arenaScratch, (size_t)f->count);
        memset(invariant, 0, (size_t)f->count);
        find_invariants(f, L, vm, numVars, defCount, defInstrIdx, invariant, arenaScratch);

        // phase 3: move safe invariants to the pre-header
        int moved = licm_move_invariants(f, L, Dom, invariant, defCount, defInstrIdx, vm, &liv);
        totalMoved += moved;

        if (moved) {
            // liveness is stale after motion: recompute before the next loop
            varmap_destroy(liv.varMap);
            arena_destroy(livArena);
            livArena = arena_create(0);
            liv = liveness_computeIr(f, NULL, NULL, livArena);
        }
    }

    varmap_destroy(liv.varMap);
    arena_destroy(livArena);
    return totalMoved > 0;
}