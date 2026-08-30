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
 *
 * ### Known limitations
 * - No copy coalescing (EaC §13.4.3): move-related vregs get a *biased coloring
 *   hint* (ra_coalesce.h) rather than full node merging.  Correct, occasionally
 *   leaves a redundant MOV that finalize() removes only when src == dst.
 * - Stack parameters (>6 arguments) not modelled: MO_STACK offsets are always
 *   negative from %rbp (spill slots); caller-stack arguments would need positive
 *   offsets.
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

/* =========================================================================
 * CFG construction for machine code
 * =========================================================================
 * The machine-code CFG is built from scratch on each iteration of the
 * regalloc loop because ra_spill_insert() may add new instructions (reload
 * temporaries, stack loads/stores) that change block boundaries.
 *
 * Block boundaries are detected the same way as in sched.c: a MACH_LABEL
 * instruction that is not at position 0 opens a new block.  Successor edges
 * are derived from the last instruction of each block.
 * ========================================================================= */

// Linear scan of the label→block mapping to resolve a jump target.
// Called only during CFG construction; the array is small (one entry per block).
static int find_label_block(const int *labelIds, const int *blockIdx,
                             int n, int labelId)
{
    for (int i = 0; i < n; i++)
        if (labelIds[i] == labelId) return blockIdx[i];
    return -1; // label not found (should not happen in well-formed code)
}

/**
 * @brief Build the machine-code CFG for @p f.
 *
 * Partitions f->instrs[] into basic blocks (split at MACH_LABEL after index 0),
 * then wires succ[] edges by examining each block's last instruction:
 *   - MACH_JMP  → single successor (the jump target).
 *   - Jcc       → two successors: fall-through (block + 1) and jump target.
 *   - MACH_RET  → no successors.
 *   - anything else → implicit fall-through to the next block.
 *
 * @param f        Machine function to analyse.
 * @param outCount Set to the number of BasicBlock entries returned.
 * @return         Heap-allocated BasicBlock array; caller must free.
 */
static BasicBlock *build_cfg(const MachFunction *f, Arena *arena, int *outCount)
{
    int cap = 8, count = 0;
    BasicBlock *blocks = malloc((size_t)cap * sizeof(BasicBlock));
    int start = 0;

    // Scan for MACH_LABEL instructions to detect block boundaries.
    for (int i = 0; i < f->count; i++) {
        if (f->instrs[i].op == MACH_LABEL && i > start) {
            if (count == cap) { cap *= 2; blocks = realloc(blocks, (size_t)cap * sizeof(BasicBlock)); }
            blocks[count++] = (BasicBlock){ start, i, {-1, -1} };
            start = i; // new block starts at the label
        }
    }
    // Close the last (or only) block.
    if (start < f->count) {
        if (count == cap) { cap *= 2; blocks = realloc(blocks, (size_t)cap * sizeof(BasicBlock)); }
        blocks[count++] = (BasicBlock){ start, f->count, {-1, -1} };
    }

    // Build a label-id → block-index lookup table for jump-target resolution.
    int *labelIds = arena_alloc(arena, (size_t)(count > 0 ? count : 1) * sizeof(int));
    int *blockIdx = arena_alloc(arena, (size_t)(count > 0 ? count : 1) * sizeof(int));
    int nLabels = 0;
    for (int b = 0; b < count; b++) {
        if (f->instrs[blocks[b].range.start].op == MACH_LABEL) {
            labelIds[nLabels] = f->instrs[blocks[b].range.start].dst.labelId;
            blockIdx[nLabels] = b;
            nLabels++;
        }
    }

    // Wire CFG edges from each block's last instruction.
    for (int b = 0; b < count; b++) {
        int last = blocks[b].range.end - 1;
        switch (f->instrs[last].op) {
        case MACH_JMP:
            // Unconditional jump: single successor = jump target.
            blocks[b].succ[0] = find_label_block(labelIds, blockIdx, nLabels,
                                                  f->instrs[last].dst.labelId);
            break;
        case MACH_JE: case MACH_JNE: case MACH_JL:
        case MACH_JLE: case MACH_JG: case MACH_JGE:
            // Conditional branch: succ[0] = fall-through, succ[1] = taken.
            blocks[b].succ[0] = (b + 1 < count) ? b + 1 : -1;
            blocks[b].succ[1] = find_label_block(labelIds, blockIdx, nLabels,
                                                  f->instrs[last].dst.labelId);
            break;
        case MACH_RET:
            // Return: no successors (succ remain -1).
            break;
        default:
            // Implicit fall-through to the next block.
            blocks[b].succ[0] = (b + 1 < count) ? b + 1 : -1;
        }
    }

    *outCount = count;
    return blocks;
}

/* =========================================================================
 * Finalisation: vreg → phys, identity-MOV removal
 * =========================================================================
 * After a successful coloring, rewrite_phys() replaces every MO_VREG
 * operand with the physical register assigned by ra_select_colors().
 * rewrite_mem() handles the two register fields inside MO_MEM addressing
 * modes (base and index).
 *
 * After substitution, any MOV that turns out to be reg→same-reg (e.g.
 * because biased coloring assigned the same color to MOV source and dest)
 * is removed by finalize() in a single compaction pass.
 * ========================================================================= */

// Replace a single MO_VREG operand with its assigned MO_PHYS color.
static inline void rewrite_phys(MachOperand *o, const int *color)
{
    if (o->kind == MO_VREG) {
        o->kind    = MO_PHYS;
        o->physReg = color[o->vregId]; // color[v] = physical register index
    }
}

// Rewrite base and index vregs inside an MO_MEM operand.
// base/index fields store the vreg id directly (not a MachOperand), so they
// are updated in-place without going through rewrite_phys().
static inline void rewrite_mem(MachOperand *o, const int *color)
{
    if (o->kind != MO_MEM) return;
    if (o->mem.baseVreg  >= 0) o->mem.baseVreg  = color[o->mem.baseVreg];
    if (o->mem.indexVreg >= 0) o->mem.indexVreg = color[o->mem.indexVreg];
}

// Rewrite all operands of every instruction and compact out identity MOVs.
static void finalize(MachFunction *f, const int *color)
{
    int newCount = 0;
    for (int i = 0; i < f->count; i++) {
        MachInstr *in = &f->instrs[i];

        // Substitute vreg operands with their physical registers.
        rewrite_phys(&in->dst,  color);
        rewrite_phys(&in->src1, color);
        rewrite_phys(&in->src2, color);

        // Substitute vreg fields inside memory addressing modes.
        rewrite_mem(&in->dst,  color);
        rewrite_mem(&in->src1, color);

        // Discard MOV physReg → same physReg (identity move, no-op).
        // These arise when biased coloring successfully assigns the same
        // physical register to both sides of a copy.
        if (in->op == MACH_MOV
            && in->dst.kind  == MO_PHYS
            && in->src1.kind == MO_PHYS
            && in->dst.physReg == in->src1.physReg)
            continue; // skip: do not copy to newCount

        f->instrs[newCount++] = *in;
    }
    f->count = newCount;
}

/* =========================================================================
 * Prologue / epilogue: callee-saved register save and restore
 * =========================================================================
 * The System V AMD64 ABI requires that RBX, R12-R15 (PHYS_RBX … PHYS_R15)
 * be preserved across calls.  After coloring, we scan the instruction stream
 * to find which of those registers were actually assigned and emit PUSH
 * instructions immediately after MACH_FUNC_BEGIN and POP instructions
 * immediately before each MACH_RET.
 *
 * This is done after finalize() so that only registers that actually ended up
 * in the output (after identity-MOV removal) are considered.
 * ========================================================================= */

// Returns true if physReg is in the callee-saved range (RBX, R12-R15).
static inline int is_callee_saved(int physReg)
{
    return physReg >= PHYS_RBX && physReg <= PHYS_R15;
}

/**
 * @brief Insert PUSH/POP pairs in the prologue/epilogue for used callee-saved regs.
 *
 * Scans f->instrs[] to build a bitmask of all callee-saved physical registers
 * that appear as destinations (written), then inserts:
 *   - A PUSH for each used register immediately after MACH_FUNC_BEGIN.
 *   - A POP for each used register (in reverse order) immediately before
 *     each MACH_RET.
 *
 * A new instruction array is heap-allocated (old array is freed).
 *
 * @param f  Machine function to patch (modified in place).
 */
static void save_restore_callee(MachFunction *f)
{
    uint32_t usedMask = 0; // bitmask of callee-saved physRegs actually written
    int retCount      = 0; // number of RET instructions (need a POP set before each)

    // First pass: find used callee-saved registers and count RETs.
    for (int i = 0; i < f->count; i++) {
        MachInstr *in = &f->instrs[i];
        if (in->dst.kind == MO_PHYS && is_callee_saved(in->dst.physReg))
            usedMask |= (1u << in->dst.physReg);
        if (in->op == MACH_RET) retCount++;
    }

    if (usedMask == 0) return; // no callee-saved registers used — nothing to do

    // Collect the used callee-saved registers in ascending order.
    int used[PHYS_CALLEE_SAVED_COUNT], usedCount = 0;
    for (int p = PHYS_RBX; p <= PHYS_R15; p++)
        if ((usedMask >> p) & 1u) used[usedCount++] = p;

    // Find MACH_FUNC_BEGIN (always present; inserted by isel_select).
    int funcBeginIdx = -1;
    for (int i = 0; i < f->count; i++)
        if (f->instrs[i].op == MACH_FUNC_BEGIN) { funcBeginIdx = i; break; }

    // Allocate a new instruction array large enough for the extra PUSH/POP.
    // Worst case: usedCount PUSHes + usedCount POPs per RET.
    MachInstr *newInstrs = malloc(
        (size_t)(f->count + usedCount + (size_t)usedCount * retCount) * sizeof(MachInstr));
    int newCount = 0;

    // Second pass: copy instructions, inserting PUSHes after FUNC_BEGIN and
    // POPs before each RET.
    for (int i = 0; i < f->count; i++) {
        if (i == funcBeginIdx) {
            // Emit FUNC_BEGIN first, then one PUSH per callee-saved register.
            newInstrs[newCount++] = f->instrs[i];
            for (int k = 0; k < usedCount; k++) {
                MachInstr ps = {0};
                ps.op = MACH_PUSH; ps.dst.kind = MO_PHYS; ps.dst.physReg = used[k];
                newInstrs[newCount++] = ps;
            }
            continue;
        }
        if (f->instrs[i].op == MACH_RET) {
            // Emit POPs in reverse order before each RET (LIFO stack discipline).
            for (int k = usedCount - 1; k >= 0; k--) {
                MachInstr po = {0};
                po.op = MACH_POP; po.dst.kind = MO_PHYS; po.dst.physReg = used[k];
                newInstrs[newCount++] = po;
            }
        }
        newInstrs[newCount++] = f->instrs[i];
    }

    free(f->instrs);
    f->instrs = newInstrs;
    f->count  = newCount;
}

/* =========================================================================
 * Main allocation loop for a single function
 * =========================================================================
 * The loop retries as long as spills occur.  Each iteration may introduce
 * new temporaries (reload/spill temps from ra_spill_insert), which change
 * the interference graph and may require another round of coloring.
 *
 * BUG FIX (PartnerList lifetime):
 *   The PartnerList must be released on BOTH the success path (nSpilled == 0)
 *   and the spill path (nSpilled > 0).  The previous code called
 *   partnerlist_free() only in the break branch, leaking `pl` on every
 *   iteration that required spilling.  Fixed by calling partnerlist_free()
 *   unconditionally before either breaking or continuing.
 *
 * FIX (optimistic-spill thrashing on reload temps):
 *   firstSpillVreg is snapshotted ONCE, before the loop starts, from the
 *   vreg id boundary handed out by instruction selection.  Every vreg id at
 *   or above this boundary — in every round, including temps introduced by
 *   ra_spill_insert() in round 1, 2, 3, ... — is a reload/spill temp, never
 *   a genuine IR-level variable.  Passing this fixed boundary to ig_build()
 *   on every round lets ra_simplify() recognise and de-prioritise these
 *   short-lived temps as optimistic-spill candidates (see interference.h),
 *   instead of repeatedly respilling the "cheapest-looking" temp while the
 *   real long-lived value causing register pressure is never addressed.
 * ========================================================================= */

static void regalloc_function(MachFunction *f)
{
    int frameOff = 0; // running stack frame offset; grows by 8 per spilled vreg

    // Fixed boundary: any vreg id >= firstSpillVreg, in any round of the
    // loop below, was introduced by ra_spill_insert() and is therefore a
    // reload/spill temp rather than a real IR-level variable/temporary.
    int firstSpillVreg = f->nextVreg;

    for (;;) {
        // --- Step 1: build machine-code CFG ---
        Arena *livArena = arena_create(0);
        int nBlocks;
        BasicBlock *blocks = build_cfg(f, livArena, &nBlocks);

        // --- Step 2: backward liveness dataflow ---
        // Produces per-instruction liveAfter[] sets needed by the interference
        // graph builder to add edges between definitions and live-at-def vars.
        LivenessResult liv = liveness_computeMach(f, blocks, nBlocks, livArena);

        // --- Step 3: build interference graph ---
        // Allocates a triangular bit matrix + adjacency lists.
        // Physical registers are pre-coloured nodes; excl[] masks and
        // crossesCall[] flags are set for vregs live across CALL/IDIV/SETcc.
        // firstSpillVreg flags every reload/spill temp so ra_simplify() can
        // avoid respilling them ahead of genuinely long-lived values.
        IGraph g = ig_build(f, blocks, nBlocks, f->nextVreg, liv.liveAfter,
                            firstSpillVreg, livArena);

        // --- Step 4: collect MOV-related vreg pairs for biased coloring ---
        // Must happen AFTER ig_build (needs g.matrix for non-interference check)
        // and BEFORE ra_simplify (which deactivates nodes, making g.matrix stale).
        PartnerList pl = ra_collect_partners(f, &g, f->nextVreg);

        // --- Step 5: Briggs-optimistic simplification ---
        // Removes nodes from the graph in bucket-by-degree order.
        // Nodes with degree < k are trivially colorable; the rest are
        // potential spill candidates selected by lowest spillCost/degree,
        // preferring non-reload-temp nodes first (see interference.h).
        int *stack    = NULL;
        int  stackLen = ra_simplify(&g, f->nextVreg, &stack);

        // --- Step 6: color assignment ---
        // Pops the stack and assigns a physical register to each node.
        // Priority: biased hint from partner → callee-saved if crossesCall → lowest available.
        // Nodes with no available color are recorded in spilled[].
        int *spilled  = malloc((size_t)(f->nextVreg > 0 ? f->nextVreg : 1) * sizeof(int));
        int  nSpilled = ra_select_colors(&g, f->nextVreg, stack, stackLen, spilled, &pl);

        // Release the partner list unconditionally — it is consumed by select_colors
        // and must be freed regardless of whether spilling occurred.
        partnerlist_free(&pl);

        if (nSpilled == 0) {
            // --- Step 7a: coloring succeeded ---
            // Substitute all vreg operands with their assigned physical registers
            // and remove any MOV that became a no-op (same src and dst).
            finalize(f, g.color);
            free(stack); free(spilled); ig_free(&g);
            arena_destroy(livArena); free(blocks);
            break; // done — exit the allocation loop
        }

        // --- Step 7b: spilling required ---
        // Insert load/store code around each spilled vreg; the resulting new
        // temporaries will be handled in the next iteration (and correctly
        // flagged isReloadTemp on all subsequent rounds, since
        // firstSpillVreg was captured once before this loop started).
        ra_spill_insert(f, spilled, nSpilled, &frameOff);
        free(stack); free(spilled); ig_free(&g);
        arena_destroy(livArena); free(blocks);
        // Loop continues: the rewritten instruction stream is re-analysed
        // from scratch (new vregs from spill code may interfere differently).
    }

    // Insert callee-saved register saves/restores in the prologue/epilogue.
    save_restore_callee(f);

    // Round frame size up to the next 16-byte boundary (ABI requirement).
    f->frameSize = (frameOff + 15) & ~15;
}

void regalloc(MachProgram *mp)
{
    // Allocate registers independently for each function in the program.
    for (int i = 0; i < mp->count; i++)
        regalloc_function(mp->functions[i]);
}
