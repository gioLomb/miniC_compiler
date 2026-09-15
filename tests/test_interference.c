#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "../arena.h"
#include "../liveness.h"
#include "../interference.h"
#include "../reg_class.h"

static MachOperand vreg(int id) { MachOperand o = {0}; o.kind = MO_VREG; o.vregId = id; return o; }
static MachOperand phys(int p)  { MachOperand o = {0}; o.kind = MO_PHYS; o.physReg = p; return o; }
static MachOperand imm(long v)  { MachOperand o = {0}; o.kind = MO_IMM;  o.imm = v; return o; }
static MachOperand mo_none(void){ MachOperand o = {0}; o.kind = MO_NONE; return o; }
static MachOperand fn(const char *name) { MachOperand o = {0}; o.kind = MO_FUNC; o.func = name; return o; }
static MachOperand label(int id) { MachOperand o = {0}; o.kind = MO_LABEL; o.labelId = id; return o; }

static inline int has_edge(const IGraph *g, int i, int j) {
    long idx;
    if (i < j) { int t = i; i = j; j = t; }
    idx = (long)i * (i - 1) / 2 + j;
    return (int)((g->matrix[idx >> 6] >> (idx & 63)) & 1ULL);
}

int main(void) {

    /* PASS 1 */
    {
        MachInstr instrs[5] = {
            { .op = MACH_MOV, .dst = vreg(0),    .src1 = imm(10),  .src2 = mo_none() },
            { .op = MACH_MOV, .dst = vreg(1),    .src1 = imm(20),  .src2 = mo_none() },
            { .op = MACH_ADD, .dst = vreg(0),    .src1 = vreg(1),  .src2 = mo_none() },
            { .op = MACH_MOV, .dst = phys(PHYS_RAX), .src1 = vreg(0), .src2 = mo_none() },
            { .op = MACH_RET, .dst = mo_none(),  .src1 = mo_none(),.src2 = mo_none() },
        };
        MachFunction f = { .name = "t1", .instrs = instrs, .count = 5,
                           .capacity = 5, .frameSize = 0, .nextVreg = 2 };
        BasicBlock blocks[1] = {{ .range = { .start = 0, .end = 5 }, .succ = {-1, -1} }};

        Arena *arena = arena_create(0);
        LivenessResult liv = liveness_computeMach(&f, blocks, 1, RC_INT, f.nextVreg, arena);
        IGraph g = ig_build(&f, blocks, 1, RC_INT, f.nextVreg, liv.liveAfter, f.nextVreg, arena);

        assert(has_edge(&g, 0, 1) &&
               "v0 and v1 must interfere: v0 is both read and written by the RMW ADD "
               "(regression Bug1 in instr_uses)");

        printf("PASS 1 ok: ADD (RMW) generates correctmente the edge v0-v1 (regression Bug1).\n");
        ig_free(&g);
        arena_destroy(arena);
    }

    /* PASS 2 */
    {
        MachInstr instrs[4] = {
            { .op = MACH_MOV,  .dst = vreg(0), .src1 = imm(1),  .src2 = mo_none() },
            { .op = MACH_CALL, .dst = mo_none(), .src1 = fn("f"), .src2 = mo_none() },
            { .op = MACH_MOV,  .dst = phys(PHYS_RAX), .src1 = vreg(0), .src2 = mo_none() },
            { .op = MACH_RET,  .dst = mo_none(), .src1 = mo_none(), .src2 = mo_none() },
        };
        MachFunction f = { .name = "t2", .instrs = instrs, .count = 4,
                           .capacity = 4, .frameSize = 0, .nextVreg = 1 };
        BasicBlock blocks[1] = {{ .range = { .start = 0, .end = 4 }, .succ = {-1, -1} }};

        Arena *arena = arena_create(0);
        LivenessResult liv = liveness_computeMach(&f, blocks, 1, RC_INT, f.nextVreg, arena);
        IGraph g = ig_build(&f, blocks, 1, RC_INT, f.nextVreg, liv.liveAfter, f.nextVreg, arena);

        assert(g.crossesCall[0] == 1);
        uint32_t callerMask = (1U << PHYS_CALLER_SAVED_COUNT) - 1;
        assert((g.excl[0] & callerMask) == callerMask &&
               "v0 live across la CALL must exclude tutti i caller-saved");

        printf("PASS 2 ok: vreg live across CALL obtains crossesCall=1 and correct excl.\n");
        ig_free(&g);
        arena_destroy(arena);
    }

    /* PASS 3 */
    {
        MachInstr instrs[5] = {
            { .op = MACH_MOV, .dst = vreg(0), .src1 = imm(1), .src2 = mo_none() },
            { .op = MACH_MOV, .dst = phys(PHYS_RAX), .src1 = vreg(0), .src2 = mo_none() },
            { .op = MACH_MOV, .dst = vreg(1), .src1 = imm(2), .src2 = mo_none() },
            { .op = MACH_MOV, .dst = phys(PHYS_RAX), .src1 = vreg(1), .src2 = mo_none() },
            { .op = MACH_RET, .dst = mo_none(), .src1 = mo_none(), .src2 = mo_none() },
        };
        MachFunction f = { .name = "t3", .instrs = instrs, .count = 5,
                           .capacity = 5, .frameSize = 0, .nextVreg = 2 };
        BasicBlock blocks[1] = {{ .range = { .start = 0, .end = 5 }, .succ = {-1, -1} }};

        Arena *arena = arena_create(0);
        LivenessResult liv = liveness_computeMach(&f, blocks, 1, RC_INT, f.nextVreg, arena);
        IGraph g = ig_build(&f, blocks, 1, RC_INT, f.nextVreg, liv.liveAfter, f.nextVreg, arena);

        assert(!has_edge(&g, 0, 1) &&
               "v0 e v1 are never live simultaneously: no interference expected");

        printf("PASS 3 ok: disjoint live ranges non generatesno spurious interference.\n");
        ig_free(&g);
        arena_destroy(arena);
    }

    /* PASS 4 */
    {
        MachInstr instrs[4] = {
            { .op = MACH_MOV,  .dst = vreg(0), .src1 = imm(7), .src2 = mo_none() },
            { .op = MACH_IDIV, .dst = vreg(0), .src1 = mo_none(), .src2 = mo_none() },
            { .op = MACH_MOV,  .dst = phys(PHYS_RAX), .src1 = vreg(0), .src2 = mo_none() },
            { .op = MACH_RET,  .dst = mo_none(), .src1 = mo_none(), .src2 = mo_none() },
        };
        MachFunction f = { .name = "t4", .instrs = instrs, .count = 4,
                           .capacity = 4, .frameSize = 0, .nextVreg = 1 };
        BasicBlock blocks[1] = {{ .range = { .start = 0, .end = 4 }, .succ = {-1, -1} }};

        Arena *arena = arena_create(0);
        LivenessResult liv = liveness_computeMach(&f, blocks, 1, RC_INT, f.nextVreg, arena);
        IGraph g = ig_build(&f, blocks, 1, RC_INT, f.nextVreg, liv.liveAfter, f.nextVreg, arena);

        uint32_t mask = (1U << PHYS_RAX) | (1U << PHYS_RDX);
        assert((g.excl[0] & mask) == mask);
        printf("PASS 4 ok: IDIV excludes RAX/RDX for the involved vreg.\n");
        ig_free(&g);
        arena_destroy(arena);
    }

    /* PASS 5 */
    {
        MachInstr instrs[4] = {
            { .op = MACH_MOV,  .dst = vreg(0), .src1 = imm(1), .src2 = mo_none() },
            { .op = MACH_SETE, .dst = phys(PHYS_AL), .src1 = mo_none(), .src2 = mo_none() },
            { .op = MACH_MOV,  .dst = phys(PHYS_RAX), .src1 = vreg(0), .src2 = mo_none() },
            { .op = MACH_RET,  .dst = mo_none(), .src1 = mo_none(), .src2 = mo_none() },
        };
        MachFunction f = { .name = "t5", .instrs = instrs, .count = 4,
                           .capacity = 4, .frameSize = 0, .nextVreg = 1 };
        BasicBlock blocks[1] = {{ .range = { .start = 0, .end = 4 }, .succ = {-1, -1} }};

        Arena *arena = arena_create(0);
        LivenessResult liv = liveness_computeMach(&f, blocks, 1, RC_INT, f.nextVreg, arena);
        IGraph g = ig_build(&f, blocks, 1, RC_INT, f.nextVreg, liv.liveAfter, f.nextVreg, arena);

        uint32_t raxMask = (1U << PHYS_RAX);
        assert((g.excl[0] & raxMask) == raxMask &&
               "v0 live across SETcc must exclude RAX (alias di %al)");
        assert(g.crossesCall[0] == 0 &&
               "SETcc is not una CALL: crossesCall non must be settato");

        printf("PASS 5 ok: SETcc excludes RAX for the vreg live across of it, without crossesCall.\n");
        ig_free(&g);
        arena_destroy(arena);
    }

    /* PASS 6 */
    {
        MachInstr instrs[8] = {
            { .op = MACH_MOV,   .dst = vreg(0), .src1 = imm(10), .src2 = mo_none() },      /* */
            { .op = MACH_JE,    .dst = label(1), .src1 = mo_none(), .src2 = mo_none() },    /* */
            { .op = MACH_ADD,   .dst = vreg(1), .src1 = vreg(0), .src2 = imm(1) },          /* */
            { .op = MACH_JMP,   .dst = label(2), .src1 = mo_none(), .src2 = mo_none() },    /* */
            { .op = MACH_LABEL, .dst = label(1), .src1 = mo_none(), .src2 = mo_none() },    /* */
            { .op = MACH_ADD,   .dst = vreg(2), .src1 = vreg(0), .src2 = imm(2) },          /* */
            { .op = MACH_LABEL, .dst = label(2), .src1 = mo_none(), .src2 = mo_none() },    /* */
            { .op = MACH_RET,   .dst = mo_none(), .src1 = mo_none(), .src2 = mo_none() },   /* */
        };
        MachFunction f = { .name = "t6", .instrs = instrs, .count = 8,
                           .capacity = 8, .frameSize = 0, .nextVreg = 3 };

        /* */
        BasicBlock blocks[4] = {
            { .range = { .start = 0, .end = 2 }, .succ = { 1, 2 } },  /* */
            { .range = { .start = 2, .end = 4 }, .succ = { 3, -1 } }, /* */
            { .range = { .start = 4, .end = 6 }, .succ = { 3, -1 } }, /* */
            { .range = { .start = 6, .end = 8 }, .succ = { -1, -1 } },
        };

        Arena *arena = arena_create(0);
        LivenessResult liv = liveness_computeMach(&f, blocks, 4, RC_INT, f.nextVreg, arena);
        IGraph g = ig_build(&f, blocks, 4, RC_INT, f.nextVreg, liv.liveAfter, f.nextVreg, arena);

        assert(has_edge(&g, 0, 1) && "v0 live at the definition of v1 in the then branch");
        assert(has_edge(&g, 0, 2) && "v0 live at the definition of v2 in the else branch");
        assert(!has_edge(&g, 1, 2) &&
               "v1 e v2 appartengono a rami mutuamente esclusivi: mai vivi insieme");

        printf("PASS 6 ok: CFG a diamante, liveness cross-block correct (join point rispettato).\n");
        ig_free(&g);
        arena_destroy(arena);
    }

    printf("\nAll interference tests passed.\n");
    return 0;
}