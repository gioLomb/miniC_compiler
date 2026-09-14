/**
 * @file ra_spill.c
 * @brief Class-aware spill insertion (GPR MOV or XMM MOVSS).
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ra_spill.h"
#include "instr_query.h"
#include "arena.h"
#include "bitset.h"

static inline int *vreg_counter_of(MachFunction *f, RegClass cls) {
    return (cls == RC_FLOAT) ? &f->fNextVreg : &f->nextVreg;
}
static inline MachOpCode mov_op_of(RegClass cls) {
    return (cls == RC_FLOAT) ? MACH_MOVSS : MACH_MOV;
}
static inline MachOperandKind vreg_kind_of(RegClass cls) {
    return (cls == RC_FLOAT) ? MO_VREG_F : MO_VREG;
}

static inline void invalidate_cache(int *cache, int n) {
    memset(cache, 0xFF, (size_t)n * sizeof(int));
}

static inline int is_spilled_vreg(const BitSet *ss, int vreg_id, int orig_next_vreg) {
    return (vreg_id >= 0 && vreg_id < orig_next_vreg && bitset_test(ss, vreg_id));
}

static void allocate_spill_slots(const int *spilled, int n_spilled, int *slot,
                                 int *frame_off, BitSet *ss) {
    for (int i = 0; i < n_spilled; i++) {
        int vreg = spilled[i];
        *frame_off += BYTES_PER_QUADWORD;
        slot[vreg] = *frame_off;
        bitset_set(ss, vreg);
    }
}

static void load_spilled(MachOperand *o, int origVreg, MachFunction *f, RegClass cls,
                         int off, MachInstr *newInstrs, int *newCount, int *cache) {
    if (cache[origVreg] >= 0) {
        o->kind   = vreg_kind_of(cls);
        o->vregId = cache[origVreg];
        return;
    }
    int tmp = (*vreg_counter_of(f, cls))++;
    MachInstr ld = {0};
    ld.op             = mov_op_of(cls);
    ld.dst.kind       = vreg_kind_of(cls); ld.dst.vregId = tmp;
    ld.src1.kind      = MO_STACK;          ld.src1.stackOff = off;
    ld.src2.kind      = MO_NONE;
    newInstrs[(*newCount)++] = ld;
    o->kind   = vreg_kind_of(cls);
    o->vregId = tmp;
    cache[origVreg] = tmp;
}

static inline void reload_operand_if_spilled(MachOperand *op, int orig_next_vreg,
                                             const BitSet *ss, const int *slot,
                                             MachFunction *f, RegClass cls,
                                             MachInstr *new_instrs, int *new_count,
                                             int *cache) {
    MachOperandKind vk = vreg_kind_of(cls);
    if (op->kind == vk && is_spilled_vreg(ss, op->vregId, orig_next_vreg))
        load_spilled(op, op->vregId, f, cls, slot[op->vregId],
                     new_instrs, new_count, cache);
}

/* Memory bases/indices are always integer vregs. */
static inline void reload_mem_operands_if_spilled(MachOperand *op, int orig_next_vreg,
                                                  const BitSet *ss, const int *slot,
                                                  MachFunction *f,
                                                  MachInstr *new_instrs, int *new_count,
                                                  int *cache) {
    if (op->kind != MO_MEM) return;
    if (op->mem.baseVreg >= 0 && is_spilled_vreg(ss, op->mem.baseVreg, orig_next_vreg)) {
        MachOperand tmp = { .kind = MO_VREG, .vregId = op->mem.baseVreg };
        load_spilled(&tmp, op->mem.baseVreg, f, RC_INT, slot[op->mem.baseVreg],
                     new_instrs, new_count, cache);
        op->mem.baseVreg = tmp.vregId;
    }
    if (op->mem.indexVreg >= 0 && is_spilled_vreg(ss, op->mem.indexVreg, orig_next_vreg)) {
        MachOperand tmp = { .kind = MO_VREG, .vregId = op->mem.indexVreg };
        load_spilled(&tmp, op->mem.indexVreg, f, RC_INT, slot[op->mem.indexVreg],
                     new_instrs, new_count, cache);
        op->mem.indexVreg = tmp.vregId;
    }
}

void ra_spill_insert(MachFunction *f, RegClass cls, const int *spilled, int nSpilled,
                     int *frameOff) {
    int origNextVreg = *vreg_counter_of(f, cls);
    MachOperandKind vk = vreg_kind_of(cls);

    int *slot = malloc((size_t)(origNextVreg > 0 ? origNextVreg : 1) * sizeof(int));
    memset(slot, -1, (size_t)(origNextVreg > 0 ? origNextVreg : 1) * sizeof(int));

    Arena *spillArena = arena_create(0);
    BitSet ss = bitset_new(spillArena, (origNextVreg + 63) / 64);

    allocate_spill_slots(spilled, nSpilled, slot, frameOff, &ss);
    int *cache = malloc((size_t)(origNextVreg > 0 ? origNextVreg : 1) * sizeof(int));
    invalidate_cache(cache, origNextVreg > 0 ? origNextVreg : 1);

    int maxNew = f->count * SPILL_MAX_EXPANSION_PER_INSTR + SPILL_EXTRA_MARGIN;
    MachInstr *newInstrs = malloc((size_t)maxNew * sizeof(MachInstr));
    int newCount = 0;

    for (int i = 0; i < f->count; i++) {
        MachInstr in = f->instrs[i];
        int isStore  = (in.op == MACH_STORE);

        invalidate_cache(cache, origNextVreg > 0 ? origNextVreg : 1);

        reload_operand_if_spilled(&in.src1, origNextVreg, &ss, slot, f, cls,
                                  newInstrs, &newCount, cache);
        reload_operand_if_spilled(&in.src2, origNextVreg, &ss, slot, f, cls,
                                  newInstrs, &newCount, cache);

        /* MEM bases are integer; only reload them during the INT spill round. */
        if (cls == RC_INT) {
            MachOperand *memHolder = isStore ? &in.dst : &in.src1;
            reload_mem_operands_if_spilled(memHolder, origNextVreg, &ss, slot, f,
                                           newInstrs, &newCount, cache);
        }

        if (in.op == MACH_CMP || in.op == MACH_TEST || in.op == MACH_UCOMISS) {
            reload_operand_if_spilled(&in.dst, origNextVreg, &ss, slot, f, cls,
                                      newInstrs, &newCount, cache);
        }

        int dstSpilled = (instr_def(&in, origNextVreg, cls) >= 0) &&
                         (in.dst.kind == vk) && !isStore &&
                         is_spilled_vreg(&ss, in.dst.vregId, origNextVreg);

        if (dstSpilled) {
            int orig = in.dst.vregId;
            int tmp = (*vreg_counter_of(f, cls))++;
            if (instr_is_rmw(in.op)) {
                MachInstr ld = {0};
                ld.op = mov_op_of(cls);
                ld.dst.kind = vreg_kind_of(cls); ld.dst.vregId = tmp;
                ld.src1.kind = MO_STACK; ld.src1.stackOff = slot[orig];
                newInstrs[newCount++] = ld;
            }
            in.dst.kind = vreg_kind_of(cls);
            in.dst.vregId = tmp;
            newInstrs[newCount++] = in;
            MachInstr st = {0};
            st.op = mov_op_of(cls);
            st.dst.kind = MO_STACK; st.dst.stackOff = slot[orig];
            st.src1.kind = vreg_kind_of(cls); st.src1.vregId = tmp;
            newInstrs[newCount++] = st;
            cache[orig] = tmp;
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
