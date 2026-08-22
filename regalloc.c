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
 * CFG costruzione per codice macchina (invariato).
 * ========================================================================= */

static int find_label_block(const int *labelIds, const int *blockIdx,
                             int n, int labelId)
{
    for (int i = 0; i < n; i++)
        if (labelIds[i] == labelId) return blockIdx[i];
    return -1;
}

static BasicBlock *build_cfg(const MachFunction *f, int *outCount)
{
    int cap = 8, count = 0;
    BasicBlock *blocks = malloc((size_t)cap * sizeof(BasicBlock));
    int start = 0;

    for (int i = 0; i < f->count; i++) {
        if (f->instrs[i].op == MACH_LABEL && i > start) {
            if (count == cap) { cap *= 2; blocks = realloc(blocks, (size_t)cap * sizeof(BasicBlock)); }
            blocks[count++] = (BasicBlock){ start, i, {-1, -1} };
            start = i;
        }
    }
    if (start < f->count) {
        if (count == cap) { cap *= 2; blocks = realloc(blocks, (size_t)cap * sizeof(BasicBlock)); }
        blocks[count++] = (BasicBlock){ start, f->count, {-1, -1} };
    }

    int *labelIds = malloc((size_t)(count > 0 ? count : 1) * sizeof(int));
    int *blockIdx = malloc((size_t)(count > 0 ? count : 1) * sizeof(int));
    int nLabels = 0;
    for (int b = 0; b < count; b++) {
        if (f->instrs[blocks[b].start].op == MACH_LABEL) {
            labelIds[nLabels] = f->instrs[blocks[b].start].dst.labelId;
            blockIdx[nLabels] = b;
            nLabels++;
        }
    }

    for (int b = 0; b < count; b++) {
        int last = blocks[b].end - 1;
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
        }
    }

    free(labelIds);
    free(blockIdx);
    *outCount = count;
    return blocks;
}

/* =========================================================================
 * Finalizzazione: colora vreg → phys, rimuove MOV identità (invariato).
 * ========================================================================= */

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

static void finalize(MachFunction *f, const int *color)
{
    int newCount = 0;
    for (int i = 0; i < f->count; i++) {
        MachInstr *in = &f->instrs[i];
        rewrite_phys(&in->dst,  color);
        rewrite_phys(&in->src1, color);
        rewrite_phys(&in->src2, color);
        rewrite_mem (&in->dst,  color);
        rewrite_mem (&in->src1, color);

        /* scarta MOV reg→stesso reg (identità) */
        if (in->op == MACH_MOV
            && in->dst.kind  == MO_PHYS
            && in->src1.kind == MO_PHYS
            && in->dst.physReg == in->src1.physReg)
            continue;

        f->instrs[newCount++] = *in;
    }
    f->count = newCount;
}

/* =========================================================================
 * Prologo/epilogo: salva/ripristina callee-saved usati (invariato).
 * ========================================================================= */

static inline int is_callee_saved(int physReg)
{
    return physReg >= PHYS_RBX && physReg <= PHYS_R15;
}

static void save_restore_callee(MachFunction *f)
{
    uint32_t usedMask = 0;
    int retCount      = 0;

    for (int i = 0; i < f->count; i++) {
        MachInstr *in = &f->instrs[i];
        if (in->dst.kind == MO_PHYS && is_callee_saved(in->dst.physReg))
            usedMask |= (1u << in->dst.physReg);
        if (in->op == MACH_RET) retCount++;
    }

    if (usedMask == 0) return;

    int used[PHYS_CALLEE_SAVED_COUNT], usedCount = 0;
    for (int p = PHYS_RBX; p <= PHYS_R15; p++)
        if ((usedMask >> p) & 1u) used[usedCount++] = p;

    int funcBeginIdx = -1;
    for (int i = 0; i < f->count; i++)
        if (f->instrs[i].op == MACH_FUNC_BEGIN) { funcBeginIdx = i; break; }

    MachInstr *newInstrs = malloc(
        (size_t)(f->count + usedCount + (size_t)usedCount * retCount) * sizeof(MachInstr));
    int newCount = 0;

    for (int i = 0; i < f->count; i++) {
        if (i == funcBeginIdx) {
            newInstrs[newCount++] = f->instrs[i];
            for (int k = 0; k < usedCount; k++) {
                MachInstr ps = {0};
                ps.op = MACH_PUSH; ps.dst.kind = MO_PHYS; ps.dst.physReg = used[k];
                newInstrs[newCount++] = ps;
            }
            continue;
        }
        if (f->instrs[i].op == MACH_RET) {
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
 * Loop principale di allocazione per singola funzione.
 *
 * Pipeline: build_cfg → liveness → IGraph → collect_partners → simplify →
 *           select_colors (con hint) → [spill → repeat] → finalize.
 *
 * BUG FIX: the PartnerList must be released in BOTH the success branch (no
 * spills) and the spill branch.  The previous code leaked `ml` on every
 * spill iteration because partnerlist_free() was only called in the break
 * path.  Corrected below: partnerlist_free() is now called unconditionally
 * before either branching out (break) or looping (spill + continue).
 * ========================================================================= */

static void regalloc_function(MachFunction *f)
{
    int frameOff = 0;

    for (;;) {
        int nBlocks;
        BasicBlock *blocks = build_cfg(f, &nBlocks);

        Arena *livArena = arena_create(0);
        LivenessResult liv = liveness_computeMach(f, blocks, nBlocks, livArena);

        IGraph g = ig_build(f, blocks, nBlocks, f->nextVreg, liv.liveAfter, livArena);

        /* Partner-list collection must happen AFTER ig_build (needs g.matrix
         * for the non-interference check) and BEFORE ra_simplify (which
         * deactivates nodes making the graph no longer fully queryable). */
        PartnerList pl = ra_collect_partners(f, &g, f->nextVreg);

        int *stack    = NULL;
        int  stackLen = ra_simplify(&g, f->nextVreg, &stack);

        int *spilled  = malloc((size_t)(f->nextVreg > 0 ? f->nextVreg : 1) * sizeof(int));
        int  nSpilled = ra_select_colors(&g, f->nextVreg, stack, stackLen, spilled, &pl);

        // release the partner list unconditionally — it is no longer needed
        // after select_colors regardless of whether spilling occurred
        partnerlist_free(&pl);

        if (nSpilled == 0) {
            // coloring succeeded: commit physical registers and exit the loop
            finalize(f, g.color);
            free(stack); free(spilled); ig_free(&g);
            arena_destroy(livArena); free(blocks);
            break;
        }

        // coloring failed for nSpilled vregs: insert spill code and retry
        ra_spill_insert(f, spilled, nSpilled, &frameOff);
        free(stack); free(spilled); ig_free(&g);
        arena_destroy(livArena); free(blocks);
        // loop continues with the rewritten instruction stream
    }

    save_restore_callee(f);
    f->frameSize = (frameOff + 15) & ~15;
}

/* =========================================================================
 * Entry point pubblico.
 * ========================================================================= */

void regalloc(MachProgram *mp)
{
    for (int i = 0; i < mp->count; i++)
        regalloc_function(mp->functions[i]);
}