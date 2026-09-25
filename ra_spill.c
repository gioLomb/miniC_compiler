#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ra_spill.h"
#include "instr_query.h"
#include "arena.h"
#include "bitset.h"

/* ---- class helpers ------------------------------------------------------ */

static inline int *vreg_counter_of(MachFunction *f, RegClass cls) {
    return (cls == RC_FLOAT) ? &f->fNextVreg : &f->nextVreg;
}

static inline MachOpCode mov_op_of(RegClass cls) {
    return (cls == RC_FLOAT) ? MACH_MOVSS : MACH_MOV;
}

static inline MachOperandKind vreg_kind_of(RegClass cls) {
    return (cls == RC_FLOAT) ? MO_VREG_F : MO_VREG;
}

/* ---- spill rewrite context ---------------------------------------------- */

/**
 * @brief Shared state for one ra_spill_insert() invocation.
 *
 * Passed by pointer to every helper so individual functions stay short and
 * do not need long parameter lists.
 */
typedef struct {
    MachFunction    *f;
    RegClass         cls;
    MachOperandKind  vk;            /**< MO_VREG or MO_VREG_F for this class. */
    int              origNextVreg;  /**< Vreg universe size before this round. */
    int             *slot;          /**< slot[v] = stack offset, or -1. */
    BitSet           ss;            /**< Set of spilled vreg ids. */
    int             *cache;         /**< Per-instruction reload-temp cache. */
    int              cacheSize;
    MachInstr       *newInstrs;
    int              newCount;
    int              maxNew;        /**< Capacity of newInstrs. */
    Arena           *arena;
} SpillCtx;

static inline void spill_emit(SpillCtx *ctx, MachInstr in) {
    ctx->newInstrs[ctx->newCount++] = in;
}

static inline int spill_fresh_temp(SpillCtx *ctx) {
    return (*vreg_counter_of(ctx->f, ctx->cls))++;
}

static inline void invalidate_cache(SpillCtx *ctx) {
    memset(ctx->cache, 0xFF, (size_t)ctx->cacheSize * sizeof(int));
}

static inline int is_spilled(const SpillCtx *ctx, int vreg_id) {
    return (vreg_id >= 0 && vreg_id < ctx->origNextVreg &&
            bitset_test(&ctx->ss, vreg_id));
}

/* ---- setup phase -------------------------------------------------------- */

static void allocate_spill_slots(const int *spilled, int n_spilled, int *slot,
                                 int *frame_off, BitSet *ss) {
    for (int i = 0; i < n_spilled; i++) {
        int v = spilled[i];
        *frame_off += BYTES_PER_QUADWORD;
        slot[v] = *frame_off;
        bitset_set(ss, v);
    }
}

/** @brief Allocate slots, bitset, reload cache and the output instruction buffer. */
static void spill_setup(SpillCtx *ctx, MachFunction *f, RegClass cls,
                        const int *spilled, int nSpilled, int *frameOff) {
    ctx->f            = f;
    ctx->cls          = cls;
    ctx->vk           = vreg_kind_of(cls);
    ctx->origNextVreg = *vreg_counter_of(f, cls);
    ctx->cacheSize    = ctx->origNextVreg > 0 ? ctx->origNextVreg : 1;
    ctx->newCount     = 0;

    ctx->slot = malloc((size_t)ctx->cacheSize * sizeof(int));
    memset(ctx->slot, -1, (size_t)ctx->cacheSize * sizeof(int));

    ctx->arena = arena_create(0);
    ctx->ss    = bitset_new(ctx->arena, (ctx->origNextVreg + 63) / 64);
    allocate_spill_slots(spilled, nSpilled, ctx->slot, frameOff, &ctx->ss);

    ctx->cache = malloc((size_t)ctx->cacheSize * sizeof(int));
    invalidate_cache(ctx);

    ctx->maxNew    = f->count * SPILL_MAX_EXPANSION_PER_INSTR + SPILL_EXTRA_MARGIN;
    ctx->newInstrs = malloc((size_t)ctx->maxNew * sizeof(MachInstr));
}

/* ---- reload helpers ----------------------------------------------------- */

/**
 * @brief Ensure @p o is a register holding the spilled value of @p origVreg.
 *
 * Hits the per-instruction cache when the same vreg was already reloaded.
 */
static void load_spilled(SpillCtx *ctx, MachOperand *o, int origVreg) {
    if (ctx->cache[origVreg] >= 0) {
        o->kind   = ctx->vk;
        o->vregId = ctx->cache[origVreg];
        return;
    }
    int tmp = spill_fresh_temp(ctx);
    MachInstr ld = {0};
    ld.op            = mov_op_of(ctx->cls);
    ld.dst.kind      = ctx->vk;
    ld.dst.vregId    = tmp;
    ld.src1.kind     = MO_STACK;
    ld.src1.stackOff = ctx->slot[origVreg];
    ld.src2.kind     = MO_NONE;
    spill_emit(ctx, ld);
    o->kind   = ctx->vk;
    o->vregId = tmp;
    ctx->cache[origVreg] = tmp;
}

static void reload_operand_if_spilled(SpillCtx *ctx, MachOperand *op) {
    if (op->kind == ctx->vk && is_spilled(ctx, op->vregId))
        load_spilled(ctx, op, op->vregId);
}

/**
 * @brief Reload spilled GPR base/index of a memory operand.
 *
 * Address registers are always integers.  Only called from the INT spill round.
 */
static void reload_address_operands(SpillCtx *ctx, MachOperand *op) {
    if (op->kind != MO_MEM) return;

    if (op->mem.baseVreg >= 0 && is_spilled(ctx, op->mem.baseVreg)) {
        MachOperand tmp = { .kind = MO_VREG, .vregId = op->mem.baseVreg };
        load_spilled(ctx, &tmp, op->mem.baseVreg);
        op->mem.baseVreg = tmp.vregId;
    }
    if (op->mem.indexVreg >= 0 && is_spilled(ctx, op->mem.indexVreg)) {
        MachOperand tmp = { .kind = MO_VREG, .vregId = op->mem.indexVreg };
        load_spilled(ctx, &tmp, op->mem.indexVreg);
        op->mem.indexVreg = tmp.vregId;
    }
}

/**
 * @brief Reload every spilled source of @p in (values and, for INT, addresses).
 */
static void reload_sources(SpillCtx *ctx, MachInstr *in) {
    int isStore = (in->op == MACH_STORE);

    reload_operand_if_spilled(ctx, &in->src1);
    reload_operand_if_spilled(ctx, &in->src2);

    /* MEM bases/indices are GPRs — only handled in the integer spill round. */
    if (ctx->cls == RC_INT) {
        MachOperand *memHolder = isStore ? &in->dst : &in->src1;
        reload_address_operands(ctx, memHolder);
    }

    /* CMP / TEST / UCOMISS also read dst; IDIV's dst is the divisor (use). */
    if (in->op == MACH_CMP || in->op == MACH_TEST || in->op == MACH_UCOMISS ||
        in->op == MACH_IDIV)
        reload_operand_if_spilled(ctx, &in->dst);

    /* MOVSS to memory: reload base/index of the address. */
    if (ctx->cls == RC_INT && in->op == MACH_MOVSS && in->dst.kind == MO_MEM)
        reload_address_operands(ctx, &in->dst);
}

/* ---- destination rewrite ------------------------------------------------ */

static int dst_is_spilled(const SpillCtx *ctx, const MachInstr *in) {
    if (in->op == MACH_STORE) return 0;
    if (in->dst.kind != ctx->vk) return 0;
    if (instr_def(in, ctx->origNextVreg, ctx->cls) < 0) return 0;
    return is_spilled(ctx, in->dst.vregId);
}

/**
 * @brief Emit @p in with a spilled destination rewritten through a fresh temp.
 *
 * RMW ops reload the old value first; pure defs only allocate the temp.
 * After the op, the result is stored back to the spill slot.
 */
static void emit_spilled_destination(SpillCtx *ctx, MachInstr in) {
    int orig = in.dst.vregId;
    int tmp;

    if (instr_is_rmw(in.op) && ctx->cache[orig] >= 0) {
        /* reload_sources already brought this vreg into a temp; reuse it so
         * dst and the matching source share one register (RMW aliasing). */
        tmp = ctx->cache[orig];
    } else {
        tmp = spill_fresh_temp(ctx);
        if (instr_is_rmw(in.op)) {
            MachInstr ld = {0};
            ld.op            = mov_op_of(ctx->cls);
            ld.dst.kind      = ctx->vk;
            ld.dst.vregId    = tmp;
            ld.src1.kind     = MO_STACK;
            ld.src1.stackOff = ctx->slot[orig];
            spill_emit(ctx, ld);
        }
    }

    in.dst.kind   = ctx->vk;
    in.dst.vregId = tmp;
    spill_emit(ctx, in);

    MachInstr st = {0};
    st.op            = mov_op_of(ctx->cls);
    st.dst.kind      = MO_STACK;
    st.dst.stackOff  = ctx->slot[orig];
    st.src1.kind     = ctx->vk;
    st.src1.vregId   = tmp;
    spill_emit(ctx, st);

    ctx->cache[orig] = tmp;
}

/* ---- rewrite phase ------------------------------------------------------ */

static void spill_rewrite(SpillCtx *ctx) {
    const MachFunction *f = ctx->f;
    for (int i = 0; i < f->count; i++) {
        MachInstr in = f->instrs[i];

        /* Cache must not persist across instructions: a store can kill the slot. */
        invalidate_cache(ctx);

        reload_sources(ctx, &in);

        if (dst_is_spilled(ctx, &in))
            emit_spilled_destination(ctx, in);
        else
            spill_emit(ctx, in);
    }
}

/* ---- commit phase ------------------------------------------------------- */

static void spill_commit(SpillCtx *ctx) {
    MachFunction *f = ctx->f;

    free(f->instrs);
    f->instrs   = ctx->newInstrs;
    f->count    = ctx->newCount;
    f->capacity = ctx->maxNew;

    free(ctx->slot);
    free(ctx->cache);
    arena_destroy(ctx->arena);
}

/* ---- public entry ------------------------------------------------------- */

void ra_spill_insert(MachFunction *f, RegClass cls, const int *spilled, int nSpilled,
                     int *frameOff) {
    SpillCtx ctx;
    spill_setup(&ctx, f, cls, spilled, nSpilled, frameOff);
    spill_rewrite(&ctx);
    spill_commit(&ctx);
}
