/**
 * @file regalloc.c
 * @brief Register allocation via graph coloring (Chaitin-Briggs, EaC §13.4).
 *
 * ### Preconditions
 * Input must be the output of isel_select() + sched_schedule(): a MachProgram
 * where every value lives in a virtual register (MO_VREG) and the frame size
 * is not yet finalised.
 *
 * ### Postconditions
 * - Every MO_VREG replaced by MO_PHYS (physical register) or MO_STACK (spill slot).
 * - MachFunction.frameSize reflects the total spill area (16-byte aligned).
 * - Prologue/epilogue push/pop every callee-saved physical register that was
 *   actually assigned during coloring.
 *
 * ### Algorithm per function (regalloc_function)
 * ```
 *   loop:
 *     1. build_cfg        — derive basic-block ranges + CFG edges from MACH_LABEL/Jcc/RET
 *     2. liveness_computeMach — backward dataflow: liveAfter[i] for each instruction
 *     3. ig_build         — interference graph (bit matrix + adjacency lists)
 *     4. ra_collect_partners — collect non-interfering MOV pairs for biased coloring
 *     5. ra_simplify      — Briggs-optimistic: remove nodes bucket-by-degree,
 *                           picking lowest spillCost/degree if no safe node exists
 *                           (reload/spill temps from earlier rounds are skipped
 *                           as optimistic-spill candidates — see interference.h)
 *     6. ra_select_colors — pop stack, assign colors; prefer partner hint, then
 *                           callee-saved if live across CALL, else lowest available
 *     7. if nSpilled == 0 → finalize() and break
 *        else             → ra_spill_insert(), free temporaries, continue
 *   save_restore_callee   — insert push/pop in prologue/epilogue for used callee-saved regs
 * ```
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "regalloc.h"
#include "ra_color.h"
#include "ra_coalesce.h"
#include "ra_spill.h"
#include "liveness.h"
#include "interference.h"
#include "regalloc_utils.h"
#include "arena.h"


static int find_label_block(const int *labelIds, const int *blockIdx,
                             int n, int labelId)
{
    for (int i = 0; i < n; i++) {
        if (labelIds[i] == labelId) {
            return blockIdx[i];
        }
    }
    return -1; // Label not found
}

/**
 * Builds the lookup table mapping label IDs to block indices.
 */
static int build_label_map(const MachFunction *f, const BasicBlock *blocks, int count,
                           Arena *arena, int **outLabelIds, int **outBlockIdx)
{
    int allocSize = (count > 0) ? count : 1;
    int *labelIds = arena_alloc(arena, (size_t)allocSize * sizeof(int));
    int *blockIdx = arena_alloc(arena, (size_t)allocSize * sizeof(int));
    int nLabels = 0;

    for (int b = 0; b < count; b++) {
        if (f->instrs[blocks[b].range.start].op == MACH_LABEL) {
            labelIds[nLabels] = f->instrs[blocks[b].range.start].dst.labelId;
            blockIdx[nLabels] = b;
            nLabels++;
        }
    }

    *outLabelIds = labelIds;
    *outBlockIdx = blockIdx;
    return nLabels;
}

/**
 * Connects control flow graph edges based on block boundary instructions.
 */
static void wire_block_successors(BasicBlock *blocks, int count, const MachFunction *f,
                                 const int *labelIds, const int *blockIdx, int nLabels)
{
    for (int b = 0; b < count; b++) {
        int last = blocks[b].range.end - 1;
        switch (f->instrs[last].op) {
        case MACH_JMP:
            blocks[b].succ[0] = find_label_block(labelIds, blockIdx, nLabels,
                                                  f->instrs[last].dst.labelId);
            break;
        case MACH_JE: case MACH_JNE: case MACH_JL:
        case MACH_JLE: case MACH_JG: case MACH_JGE:
            blocks[b].succ[0] = (b + 1 < count) ? b + 1 : -1;
            blocks[b].succ[1] = find_label_block(labelIds, blockIdx, nLabels,
                                                  f->instrs[last].dst.labelId);
            break;
        case MACH_RET:
            break;
        default:
            blocks[b].succ[0] = (b + 1 < count) ? b + 1 : -1;
            break;
        }
    }
}

/**
 * @brief Build the machine-code CFG for @p f.
 */
static BasicBlock *build_cfg(const MachFunction *f, Arena *arena, int *outCount)
{
    int cap = INITIAL_BLOCK_CAPACITY;
    int count = 0;
    BasicBlock *blocks = malloc((size_t)cap * sizeof(BasicBlock));
    int start = 0;

    // Partition instructions into basic blocks
    for (int i = 0; i < f->count; i++) {
        if (f->instrs[i].op == MACH_LABEL && i > start) {
            if (count == cap) {
                cap *= 2;
                blocks = realloc(blocks, (size_t)cap * sizeof(BasicBlock));
            }
            blocks[count++] = (BasicBlock){ start, i, {-1, -1} };
            start = i;
        }
    }

    if (start < f->count) {
        if (count == cap) {
            cap *= 2;
            blocks = realloc(blocks, (size_t)cap * sizeof(BasicBlock));
        }
        blocks[count++] = (BasicBlock){ start, f->count, {-1, -1} };
    }

    // Build label lookup table and wire CFG edges (Refactoring: Extract Function)
    int *labelIds = NULL;
    int *blockIdx = NULL;
    int nLabels = build_label_map(f, blocks, count, arena, &labelIds, &blockIdx);

    wire_block_successors(blocks, count, f, labelIds, blockIdx, nLabels);

    *outCount = count;
    return blocks;
}

static inline void rewrite_phys(MachOperand *o, const int *color)
{
    if (o->kind == MO_VREG) {
        o->kind    = MO_PHYS;
        o->physReg = color[o->vregId];
    }
}

static inline void rewrite_mem(MachOperand *o, const int *color)
{
    if (o->kind != MO_MEM) return;
    if (o->mem.baseVreg  >= 0) o->mem.baseVreg  = color[o->mem.baseVreg];
    if (o->mem.indexVreg >= 0) o->mem.indexVreg = color[o->mem.indexVreg];
}

/**
 * Predicate checking if an instruction is a redundant self-move operation.
 */
static inline int is_identity_mov(const MachInstr *in)
{
    return (in->op == MACH_MOV &&
            in->dst.kind == MO_PHYS &&
            in->src1.kind == MO_PHYS &&
            in->dst.physReg == in->src1.physReg);
}

static void finalize(MachFunction *f, const int *color)
{
    const int count = f->count;
    MachInstr *instrs = f->instrs;
    int newCount = 0;

    for (int i = 0; i < count; i++) {
        MachInstr *in = &instrs[i];

        rewrite_phys(&in->dst,  color);
        rewrite_phys(&in->src1, color);
        rewrite_phys(&in->src2, color);

        rewrite_mem(&in->dst,  color);
        rewrite_mem(&in->src1, color);

        if (!is_identity_mov(in)) {
            instrs[newCount++] = *in;
        }
    }

    f->count = newCount;
}


static inline int is_callee_saved(int physReg)
{
    return physReg >= PHYS_RBX && physReg <= PHYS_R15;
}

/**
 * Scans instructions to identify which callee-saved physical registers were written.
 */
static uint32_t collect_used_callee_saved(const MachFunction *f, int *outRetCount)
{
    uint32_t usedMask = 0;
    int retCount = 0;

    for (int i = 0; i < f->count; i++) {
        const MachInstr *in = &f->instrs[i];
        if (in->dst.kind == MO_PHYS && is_callee_saved(in->dst.physReg)) {
            usedMask |= (1u << in->dst.physReg);
        }
        if (in->op == MACH_RET) {
            retCount++;
        }
    }

    *outRetCount = retCount;
    return usedMask;
}

/**
 * Finds the index of the MACH_FUNC_BEGIN instruction.
 */
static int find_func_begin_index(const MachFunction *f)
{
    for (int i = 0; i < f->count; i++) {
        if (f->instrs[i].op == MACH_FUNC_BEGIN) {
            return i;
        }
    }
    return -1;
}

/**
 * @brief Insert PUSH/POP pairs in the prologue/epilogue for used callee-saved regs.
 */ //TODO: ARENA, CACHING POINTER, EXTRACT FUNCTION
static void save_restore_callee(MachFunction *f){
    int retCount = 0;
    uint32_t usedMask = collect_used_callee_saved(f, &retCount);

    if (usedMask == 0) return; // Guard clause: No registers to save

    int used[PHYS_CALLEE_SAVED_COUNT];
    int usedCount = 0;

    for (int p = PHYS_RBX; p <= PHYS_R15; p++) {
        if ((usedMask >> p) & 1u) {
            used[usedCount++] = p;
        }
    }

    int funcBeginIdx = find_func_begin_index(f);

    size_t allocCapacity = (size_t)(f->count + usedCount + (size_t)usedCount * retCount);
    MachInstr *newInstrs = malloc(allocCapacity * sizeof(MachInstr));
    int newCount = 0;

    for (int i = 0; i < f->count; i++) {
        if (i == funcBeginIdx) {
            newInstrs[newCount++] = f->instrs[i];
            for (int k = 0; k < usedCount; k++) {
                MachInstr ps = {0};
                ps.op = MACH_PUSH;
                ps.dst.kind = MO_PHYS;
                ps.dst.physReg = used[k];
                newInstrs[newCount++] = ps;
            }
            continue;
        }

        if (f->instrs[i].op == MACH_RET) {
            for (int k = usedCount - 1; k >= 0; k--) {
                MachInstr po = {0};
                po.op = MACH_POP;
                po.dst.kind = MO_PHYS;
                po.dst.physReg = used[k];
                newInstrs[newCount++] = po;
            }
        }

        newInstrs[newCount++] = f->instrs[i];
    }

    free(f->instrs);
    f->instrs = newInstrs;
    f->count  = newCount;
}


/**
 * @brief Run a single build->liveness->interference->color round.
 *
 * On success (no spills), applies the coloring via finalize() and returns 1.
 * On failure, inserts spill/reload code via ra_spill_insert() (which the
 * caller must re-analyse in a subsequent round) and returns 0.
 *
 * @param f              Machine function being allocated (modified in place).
 * @param firstSpillVreg  Fixed boundary: vreg ids at or above this value are
 *                        reload/spill temps from a previous round, never a
 *                        genuine IR-level variable (see interference.h).
 * @param frameOff       Running stack frame offset; grows by 8 per spilled
 *                        vreg inserted this round.
 * @return               1 if coloring succeeded (caller should stop looping),
 *                        0 if a spill round was performed (caller must retry).
 */
static int regalloc_try_round(MachFunction *f, int firstSpillVreg, int *frameOff){

    Arena *livArena = arena_create(0);
    int nBlocks;
    BasicBlock *blocks = build_cfg(f, livArena, &nBlocks);

    // Backward liveness analysis
    LivenessResult liv = liveness_computeMach(f, blocks, nBlocks, livArena);
    IGraph g = ig_build(f, blocks, nBlocks, f->nextVreg, liv.liveAfter,
                        firstSpillVreg, livArena);

    //  Collect MOV-related vreg pairs for biased coloring
    PartnerList pl = ra_collect_partners(f, &g, f->nextVreg);

    // Simplification phase
    int *stack = NULL;
    int stackLen = ra_simplify(&g, f->nextVreg, &stack);

    int *spilled = malloc((size_t)(f->nextVreg > 0 ? f->nextVreg : 1) * sizeof(int));
    int nSpilled = ra_select_colors(&g, stack, stackLen, spilled, &pl);

    partnerlist_free(&pl);

    // coloring succeeded, or spilling required
    int done = (nSpilled == 0);
    if (done) {
        finalize(f, g.color);
    } else {
        // Spill handling — insert load/store instructions for next round
        ra_spill_insert(f, spilled, nSpilled, frameOff);
    }


    free(stack);
    free(spilled);
    ig_free(&g);
    arena_destroy(livArena);
    free(blocks);

    return done;
}

static void regalloc_function(MachFunction *f)
{
    int frameOff = 0;
    int firstSpillVreg = f->nextVreg;

    // Retry rounds until one colors without spilling.
    while (!regalloc_try_round(f, firstSpillVreg, &frameOff));

    // Save/restore callee-saved registers in prologue/epilogue
    save_restore_callee(f);

    // Align frame size to 16-byte boundary (Refactoring: Replace Magic Literal / Explaining Variable)
    f->frameSize = (frameOff + (STACK_ALIGNMENT_BYTES - 1)) & ~(STACK_ALIGNMENT_BYTES - 1);
}

void regalloc(MachProgram *mp)
{
    for (int i = 0; i < mp->count; i++) {
        regalloc_function(mp->functions[i]);
    }
}