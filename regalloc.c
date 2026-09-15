#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "regalloc.h"
#include "ra_color.h"
#include "ra_coalesce.h"
#include "ra_spill.h"
#include "reg_class.h"
#include "liveness.h"
#include "interference.h"
#include "regalloc_utils.h"
#include "arena.h"


#include <limits.h>   // INT_MAX/INT_MIN per il calcolo del range label

/** Sentinel: nessun blocco registrato per questo slot label. */
#define LABEL_MAP_NONE (-1)

/**
 * @brief Build a direct-address labelId -> block-index map for @p blocks.
 *
 * Label ids seen in one function occupy a compact integer range (they come
 * from ir.c's globally increasing nextLabel counter, and this function only
 * sees the slice generated while it was being compiled), so a direct-address
 * array offset by the minimum label id gives O(1) lookup after a single
 * O(nBlocks) build pass — replacing the previous find_label_block() linear
 * scan called once per JMP/Jcc (O(nBlocks) per call, O(nBlocks^2) overall
 * per regalloc_wire_block_successors() invocation, repeated every regalloc round).
 *
 * @param f        Machine function whose label-starting blocks are indexed
 *                 (a block's label, if any, always sits at range.start).
 * @param blocks   Basic-block array.
 * @param count    Number of blocks.
 * @param arena    Arena for the output array.
 * @param outMinId Set to the minimum label id found (lookup offset), or 0
 *                 if the function has no labels at all.
 * @param outSize  Set to the allocated size of the returned array.
 * @return         Arena-allocated array; slot [labelId - *outMinId] holds
 *                 the owning block index, or LABEL_MAP_NONE if unmapped.
 */
static int *regalloc_build_label_to_block(const MachFunction *f, const BasicBlock *blocks, int count,
                                  Arena *arena, int *outMinId, int *outSize)
{
    int minId = INT_MAX, maxId = INT_MIN;

    // first pass: find the label id range among this function's blocks
    for (int b = 0; b < count; b++) {
        if (f->instrs[blocks[b].range.start].op == MACH_LABEL) {
            int lid = f->instrs[blocks[b].range.start].dst.labelId;
            if (lid < minId) minId = lid;
            if (lid > maxId) maxId = lid;
        }
    }

    if (minId > maxId) {
        // no labels at all in this function (e.g. a single straight-line block)
        *outMinId = 0;
        *outSize  = 1;
        int *map = arena_alloc(arena, sizeof(int));
        map[0] = LABEL_MAP_NONE;
        return map;
    }

    int size = maxId - minId + 1;
    int *map = arena_alloc(arena, (size_t)size * sizeof(int));
    for (int i = 0; i < size; i++) map[i] = LABEL_MAP_NONE;

    // second pass: fill direct-address slots
    for (int b = 0; b < count; b++) {
        if (f->instrs[blocks[b].range.start].op == MACH_LABEL) {
            int lid = f->instrs[blocks[b].range.start].dst.labelId;
            int slot = lid - minId;
            if (map[slot] == LABEL_MAP_NONE)
                map[slot] = b;
        }
    }

    *outMinId = minId;
    *outSize  = size;
    return map;
}

/** @brief O(1) labelId -> block-index lookup via the direct-address map. */
static inline int lookup_label_block(const int *map, int minId, int size, int labelId)
{
    int idx = labelId - minId;
    return (idx >= 0 && idx < size) ? map[idx] : -1;
}

/**
 * Connects control flow graph edges based on block boundary instructions.
 */
static void regalloc_wire_block_successors(BasicBlock *blocks, int count, const MachFunction *f,
                                 const int *labelMap, int minId, int mapSize){
    for (int b = 0; b < count; b++) {
        int last = blocks[b].range.end - 1;
        switch (f->instrs[last].op) {
        case MACH_JMP:
            blocks[b].succ[0] = lookup_label_block(labelMap, minId, mapSize,
                                                    f->instrs[last].dst.labelId);
            break;
        case MACH_JE: case MACH_JNE: case MACH_JL:
        case MACH_JLE: case MACH_JG: case MACH_JGE:
        case MACH_JB:  case MACH_JBE: case MACH_JA: case MACH_JAE:
            blocks[b].succ[0] = (b + 1 < count) ? b + 1 : -1;
            blocks[b].succ[1] = lookup_label_block(labelMap, minId, mapSize,
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
static BasicBlock *regalloc_build_cfg(const MachFunction *f, Arena *arena, int *outCount)
{
    int cap = INITIAL_BLOCK_CAPACITY;
    int count = 0;
    BasicBlock *blocks = malloc((size_t)cap * sizeof(BasicBlock));
    int start = 0;

    for (int i = 0; i < f->count; i++) {
        MachOpCode op = f->instrs[i].op;
        int isLabelSplit = (op == MACH_LABEL && i > start);
        int isCtrlSplit  = instr_is_ctrl_transfer(op);
        if (!isLabelSplit && !isCtrlSplit) continue;

        if (count == cap) {
            cap *= 2;
            blocks = realloc(blocks, (size_t)cap * sizeof(BasicBlock));
        }
        // control-transfer closes [start, i+1) (branch stays IN the block);
        // label closes [start, i) and the label opens the next block
        int end = isLabelSplit ? i : i + 1;
        blocks[count++] = (BasicBlock){ .range = { start, end }, .succ = {-1, -1} };
        start = end;
    }

    if (start < f->count) {
        if (count == cap) { cap *= 2; blocks = realloc(blocks, (size_t)cap * sizeof(BasicBlock)); }
        blocks[count++] = (BasicBlock){ .range = { start, f->count }, .succ = {-1, -1} };
    }

    int minId, mapSize;
    int *labelMap = regalloc_build_label_to_block(f, blocks, count, arena, &minId, &mapSize);
    regalloc_wire_block_successors(blocks, count, f, labelMap, minId, mapSize);

    *outCount = count;
    return blocks;
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
/** @brief Build a PUSH instruction saving physical register @p physReg. */
static inline MachInstr make_push_instr(int physReg) {
    MachInstr ps = {0};
    ps.op          = MACH_PUSH;
    ps.dst.kind    = MO_PHYS;
    ps.dst.physReg = physReg;
    return ps;
}

/** @brief Build a POP instruction restoring physical register @p physReg. */
static inline MachInstr make_pop_instr(int physReg) {
    MachInstr po = {0};
    po.op          = MACH_POP;
    po.dst.kind    = MO_PHYS;
    po.dst.physReg = physReg;
    return po;
}

/**
 * @brief Emit @p funcBeginInstr followed by one PUSH per used callee-saved reg.
 * @return Updated write cursor into @p dst.
 */
static int emit_pushes(MachInstr *dst, int at, const MachInstr *funcBeginInstr,
                        const int *used, int usedCount) {
    // keep FUNC_BEGIN itself as the very first instruction
    dst[at++] = *funcBeginInstr;
    // one PUSH per callee-saved reg actually written in this function
    for (int k = 0; k < usedCount; k++)
        dst[at++] = make_push_instr(used[k]);
    return at;
}

/**
 * @brief Emit POPs for the used callee-saved registers, reverse push order.
 * @return Updated write cursor into @p dst.
 */
static int emit_pops(MachInstr *dst, int at, const int *used, int usedCount) {
    // LIFO: last pushed must be popped first to restore correctly
    for (int k = usedCount - 1; k >= 0; k--)
        dst[at++] = make_pop_instr(used[k]);
    return at;
}

static void regalloc_save_restore_callee(MachFunction *f)
{
    int retCount = 0;
    uint32_t usedMask = collect_used_callee_saved(f, &retCount);

    if (usedMask == 0) return; // no callee-saved reg touched: nothing to save

    int used[PHYS_CALLEE_SAVED_COUNT];
    int usedCount = 0;
    // collect used callee-saved regs in fixed RBX..R15 order (deterministic push/pop order)
    for (int p = PHYS_RBX; p <= PHYS_R15; p++)
        if ((usedMask >> p) & 1u) used[usedCount++] = p;

    int funcBeginIdx = find_func_begin_index(f);

    // worst case: 1 PUSH per used reg once, 1 POP per used reg per RET
    size_t allocCapacity = (size_t)(f->count + usedCount + (size_t)usedCount * retCount);
    MachInstr *newInstrs = malloc(allocCapacity * sizeof(MachInstr));
    int newCount = 0;

    for (int i = 0; i < f->count; i++) {
        if (i == funcBeginIdx) {
            // prologue site: FUNC_BEGIN + pushes, then skip the plain copy below
            newCount = emit_pushes(newInstrs, newCount, &f->instrs[i], used, usedCount);
            continue;
        }
        if (f->instrs[i].op == MACH_RET)
            // epilogue site: pops go right before RET
            newCount = emit_pops(newInstrs, newCount, used, usedCount);

        // default: copy the instruction through unchanged
        newInstrs[newCount++] = f->instrs[i];
    }

    free(f->instrs); // old buffer always malloc'd (mfunc_create/realloc pipeline)
    f->instrs = newInstrs;
    f->count  = newCount;
}
/**
 * @brief Run a single build->liveness->interference->color round.
 *
 * On success (no spills), applies the coloring via regalloc_finalize() and returns 1.
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
static void regalloc_finalize_int(MachFunction *f, const int *color) {
    /* Rewrite MO_VREG -> MO_PHYS; rewrite MO_MEM bases (always int). */
    const int count = f->count;
    MachInstr *instrs = f->instrs;
    int newCount = 0;
    for (int i = 0; i < count; i++) {
        MachInstr *in = &instrs[i];
        if (in->dst.kind == MO_VREG) {
            in->dst.kind = MO_PHYS;
            in->dst.physReg = color[in->dst.vregId];
        }
        if (in->src1.kind == MO_VREG) {
            in->src1.kind = MO_PHYS;
            in->src1.physReg = color[in->src1.vregId];
        }
        if (in->src2.kind == MO_VREG) {
            in->src2.kind = MO_PHYS;
            in->src2.physReg = color[in->src2.vregId];
        }
        if (in->dst.kind == MO_MEM && in->dst.mem.baseVreg >= 0)
            in->dst.mem.baseVreg = color[in->dst.mem.baseVreg];
        if (in->src1.kind == MO_MEM && in->src1.mem.baseVreg >= 0)
            in->src1.mem.baseVreg = color[in->src1.mem.baseVreg];
        if (in->dst.kind == MO_MEM && in->dst.mem.indexVreg >= 0)
            in->dst.mem.indexVreg = color[in->dst.mem.indexVreg];
        if (in->src1.kind == MO_MEM && in->src1.mem.indexVreg >= 0)
            in->src1.mem.indexVreg = color[in->src1.mem.indexVreg];
        int identity = (in->op == MACH_MOV && in->dst.kind == MO_PHYS &&
                        in->src1.kind == MO_PHYS && in->dst.physReg == in->src1.physReg);
        if (!identity) instrs[newCount++] = *in;
    }
    f->count = newCount;
}

static void regalloc_finalize_float(MachFunction *f, const int *fcolor) {
    const int count = f->count;
    MachInstr *instrs = f->instrs;
    int newCount = 0;
    for (int i = 0; i < count; i++) {
        MachInstr *in = &instrs[i];
        if (in->dst.kind == MO_VREG_F) {
            in->dst.kind = MO_PHYS;
            in->dst.physReg = PHYS_XMM0 + fcolor[in->dst.vregId];
        }
        if (in->src1.kind == MO_VREG_F) {
            in->src1.kind = MO_PHYS;
            in->src1.physReg = PHYS_XMM0 + fcolor[in->src1.vregId];
        }
        if (in->src2.kind == MO_VREG_F) {
            in->src2.kind = MO_PHYS;
            in->src2.physReg = PHYS_XMM0 + fcolor[in->src2.vregId];
        }
        int identity = (in->op == MACH_MOVSS && in->dst.kind == MO_PHYS &&
                        in->src1.kind == MO_PHYS && in->dst.physReg == in->src1.physReg);
        if (!identity) instrs[newCount++] = *in;
    }
    f->count = newCount;
}

/**
 * One coloring attempt for a single register class.
 * @return 1 on success (no spills), 0 if spill code was inserted.
 */
/**
 * One coloring attempt for a single register class.
 * @return 1 on success (no spills), 0 if spill code was inserted.
 */
static int regalloc_try_round(MachFunction *f, RegClass cls, int *pVregCount,
                              int firstSpillVreg, int *frameOff) {
    // Snapshot *pVregCount at round entry: this is the vreg universe size to
    // build liveness/interference/coloring against for THIS round. If this
    // round fails and spills, ra_spill_insert bumps *pVregCount in place
    // (spill_fresh_temp -> (*vreg_counter_of(f, cls))++, same f->nextVreg /
    // f->fNextVreg cell pVregCount points at). The NEXT call to
    // regalloc_try_round re-reads through the pointer here and picks up the
    // grown count -> graph/bitsets always sized to match the vregs actually
    // referenced by instructions, no more stale snapshot -> no more OOB write
    // in ig_add_edge / liveness bitsets.
    int classVregCount = *pVregCount;

    Arena *livArena = arena_create(0);
    int nBlocks;
    BasicBlock *blocks = regalloc_build_cfg(f, livArena, &nBlocks);

    LivenessResult liv = liveness_computeMach(f, blocks, nBlocks, cls, classVregCount, livArena);
    IGraph g = ig_build(f, blocks, nBlocks, cls, classVregCount, liv.liveAfter,
                        firstSpillVreg, livArena);

    PartnerList pl = ra_collect_partners(f, &g, cls, classVregCount);

    int *stack = NULL;
    const RegClassInfo *ci = reg_class_info(cls);
    int stackLen = ra_simplify(&g, classVregCount, ci->allocatable, &stack);

    int *spilled = malloc((size_t)(classVregCount > 0 ? classVregCount : 1) * sizeof(int));
    int nSpilled = ra_select_colors(&g, stack, stackLen, ci->allocatable,
                                    ci->callerSavedCount, spilled, &pl);
    partnerlist_free(&pl);

    int done = (nSpilled == 0);
    if (done) {
        if (cls == RC_INT) regalloc_finalize_int(f, g.color);
        else               regalloc_finalize_float(f, g.color);
    } else {
        ra_spill_insert(f, cls, spilled, nSpilled, frameOff); // bumps *pVregCount in place for next round
    }

    free(stack);
    free(spilled);
    ig_free(&g);
    arena_destroy(livArena);
    free(blocks);
    return done;
}

static int regalloc_class(MachFunction *f, RegClass cls,
                          int *pVregCount, int *frameOff) {

    int firstSpillVreg = *pVregCount;

    for (int round = 0; round < REGALLOC_MAX_ROUNDS; round++) {
        if (regalloc_try_round(f, cls, pVregCount, firstSpillVreg, frameOff))
            return 1;
    }
    return 0;
}

static void regalloc_function(MachFunction *f) {
    int frameOff = 0;

    regalloc_class(f, RC_INT,   &f->nextVreg,  &frameOff);
    regalloc_class(f, RC_FLOAT, &f->fNextVreg, &frameOff);

    regalloc_save_restore_callee(f); /* no XMM is callee-saved */

    int retCount = 0;
    uint32_t usedMask = collect_used_callee_saved(f, &retCount);
    int nCalleeSaved = __builtin_popcount(usedMask);

    int frameSize = (frameOff + (STACK_ALIGNMENT_BYTES - 1)) & ~(STACK_ALIGNMENT_BYTES - 1);
    if (nCalleeSaved & 1)
        frameSize += 8;
    f->frameSize = frameSize;
}

void regalloc(MachProgram *mp)
{
    for (int i = 0; i < mp->count; i++) {
        regalloc_function(mp->functions[i]);
    }
}