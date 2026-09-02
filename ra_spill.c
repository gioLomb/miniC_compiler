#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ra_spill.h"
#include "regalloc_utils.h"   
#include "arena.h"
#include "bitset.h"           



static inline void invalidate_cache(int *cache, int n)
{
    memset(cache, 0xFF, (size_t)n * sizeof(int));
}

/**
 * Checks whether a given virtual register ID is a valid candidate and marked as spilled.
 */
static inline int is_spilled_vreg(const BitSet *ss, int vreg_id, int orig_next_vreg)
{
    return (vreg_id >= 0 && vreg_id < orig_next_vreg && bitset_test(ss, vreg_id));
}

/**
 * Allocates stack frame offsets and populates the spill BitSet in a single pass.
 * (Refactoring: Extract Function & Slide Statements)
 */
static void allocate_spill_slots(const int *spilled, int n_spilled, int *slot, 
                                 int *frame_off, BitSet *ss)
{
    for (int i = 0; i < n_spilled; i++) {
        int vreg = spilled[i];
        *frame_off += BYTES_PER_QUADWORD;
        slot[vreg] = *frame_off;
        bitset_set(ss, vreg);
    }
}

/**
 * Load spilled vreg 'origVreg' from stack slot 'off' into a temporary,
 * reusing the cached temp if already loaded earlier for the SAME original instruction.
 */
static void load_spilled(MachOperand *o, int origVreg, MachFunction *f,
                         int off, MachInstr *newInstrs, int *newCount,
                         int *cache)
{
    // Cache hit: slot reloaded earlier for the same instruction
    if (cache[origVreg] >= 0) {
        o->kind   = MO_VREG;
        o->vregId = cache[origVreg];
        return;
    }

    // Cache miss: allocate fresh temp and emit stack load
    int tmp = f->nextVreg++;
    MachInstr ld      = {0};
    ld.op             = MACH_MOV;
    ld.dst.kind       = MO_VREG;  ld.dst.vregId   = tmp;
    ld.src1.kind      = MO_STACK; ld.src1.stackOff = off;
    ld.src2.kind      = MO_NONE;

    newInstrs[(*newCount)++] = ld;
    o->kind   = MO_VREG;
    o->vregId = tmp;
    cache[origVreg] = tmp;
}

/* =========================================================================
 * Sub-pipeline Helpers for Instruction Rewriting (Refactoring: Extract Function)
 * ========================================================================= */

static inline void reload_operand_if_spilled(MachOperand *op, int orig_next_vreg,
                                             const BitSet *ss, const int *slot,
                                             MachFunction *f, MachInstr *new_instrs,
                                             int *new_count, int *cache)
{
    if (op->kind == MO_VREG && is_spilled_vreg(ss, op->vregId, orig_next_vreg)) {
        int orig = op->vregId;
        load_spilled(op, orig, f, slot[orig], new_instrs, new_count, cache);
    }
}

static inline void reload_mem_operands_if_spilled(MachOperand *mem_holder, int orig_next_vreg,
                                                 const BitSet *ss, const int *slot,
                                                 MachFunction *f, MachInstr *new_instrs,
                                                 int *new_count, int *cache)
{
    if (mem_holder->kind != MO_MEM) return;

    if (is_spilled_vreg(ss, mem_holder->mem.baseVreg, orig_next_vreg)) {
        int orig = mem_holder->mem.baseVreg;
        MachOperand tmp = { .kind = MO_VREG, .vregId = orig };
        load_spilled(&tmp, orig, f, slot[orig], new_instrs, new_count, cache);
        mem_holder->mem.baseVreg = tmp.vregId;
    }

    if (is_spilled_vreg(ss, mem_holder->mem.indexVreg, orig_next_vreg)) {
        int orig = mem_holder->mem.indexVreg;
        MachOperand tmp = { .kind = MO_VREG, .vregId = orig };
        load_spilled(&tmp, orig, f, slot[orig], new_instrs, new_count, cache);
        mem_holder->mem.indexVreg = tmp.vregId;
    }
}

static inline void emit_spilled_destination(MachInstr *in, int orig_next_vreg,
                                            const BitSet *ss, const int *slot,
                                            MachFunction *f, MachInstr *new_instrs,
                                            int *new_count, int *cache)
{
    int orig_dst_vreg = in->dst.vregId;
    int dst_store_off = slot[orig_dst_vreg];
    int dst_tmp;

    if (regalloc_is_rmw(in->op)) {
        // Read-modify-write: load current value before instruction executes
        MachOperand tmp = { .kind = MO_VREG, .vregId = orig_dst_vreg };
        load_spilled(&tmp, orig_dst_vreg, f, dst_store_off, new_instrs, new_count, cache);
        dst_tmp = tmp.vregId;
    } else {
        // Pure write: allocate a fresh temporary
        dst_tmp = f->nextVreg++;
    }

    in->dst.vregId = dst_tmp;
    new_instrs[(*new_count)++] = *in;

    // Emit store instruction to persist result to stack frame
    MachInstr st = {0};
    st.op         = MACH_MOV;
    st.dst.kind   = MO_STACK; 
    st.dst.stackOff = dst_store_off;
    st.src1.kind  = MO_VREG;  
    st.src1.vregId  = dst_tmp;
    st.src2.kind  = MO_NONE;

    new_instrs[(*new_count)++] = st;
    cache[orig_dst_vreg] = dst_tmp;
}


void ra_spill_insert(MachFunction *f, const int *spilled, int nSpilled,
                     int *frameOff)
{
    int origNextVreg = f->nextVreg;

    int *slot = malloc((size_t)origNextVreg * sizeof(int));
    memset(slot, -1, (size_t)origNextVreg * sizeof(int));

    Arena *spillArena = arena_create(0);
    BitSet ss = bitset_new(spillArena, (origNextVreg + 63) / 64);

    allocate_spill_slots(spilled, nSpilled, slot, frameOff, &ss);
    int *cache = malloc((size_t)origNextVreg * sizeof(int));
    invalidate_cache(cache, origNextVreg);

    int maxNew = f->count * SPILL_MAX_EXPANSION_PER_INSTR + SPILL_EXTRA_MARGIN;
    MachInstr *newInstrs = malloc((size_t)maxNew * sizeof(MachInstr));
    int newCount = 0;

    for (int i = 0; i < f->count; i++) {
        MachInstr in = f->instrs[i];
        int isStore  = (in.op == MACH_STORE);

        // Reset reload cache before every instruction (per-instruction scope limit)
        invalidate_cache(cache, origNextVreg);

        /* Reload spilled sources */
        reload_operand_if_spilled(&in.src1, origNextVreg, &ss, slot, f, newInstrs, &newCount, cache);
        reload_operand_if_spilled(&in.src2, origNextVreg, &ss, slot, f, newInstrs, &newCount, cache);

        /* Reload base/index of MO_MEM operand */
        MachOperand *memHolder = isStore ? &in.dst : &in.src1;
        reload_mem_operands_if_spilled(memHolder, origNextVreg, &ss, slot, f, newInstrs, &newCount, cache);

        /* Handle destination (spilled vs unspilled) */
        int dstSpilled = (in.dst.kind == MO_VREG) && !isStore &&
                          is_spilled_vreg(&ss, in.dst.vregId, origNextVreg);

        if (dstSpilled) {
            emit_spilled_destination(&in, origNextVreg, &ss, slot, f, newInstrs, &newCount, cache);
        } else {
            newInstrs[newCount++] = in;
        }
    }

    free(f->instrs);
    f->instrs   = newInstrs;
    f->count    = newCount;
    f->capacity = maxNew;

    free(slot);
    free(cache);
    arena_destroy(spillArena);
}