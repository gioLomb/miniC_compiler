/**
 * @file test_interference.c
 * @brief Unit test end-to-end (liveness_computeMach + ig_build) per interference.c.
 *
 * A differenza di test_regalloc_utils.c (che testa instr_uses/defs in
 * isolamento), qui si costruisce un MachFunction sintetico completo per
 * verificare che l'errore di liveness dovuto al Bug 1 (dst di una RMW non
 * trattato come use) si propaghi davvero nell'assenza di un arco nel grafo
 * di interferenza -- il sintomo osservabile che avrebbe causato la
 * corruzione silenziosa di registri nell'allocatore.
 *
 * Ogni chiamata a ig_build() passa firstSpillVreg = f.nextVreg: nei
 * MachFunction sintetici qui costruiti non esistono reload/spill temp
 * (nessun ra_spill_insert() e' mai girato), quindi nessun vreg deve essere
 * marcato isReloadTemp.
 *
 * PASS 1-4: casi originali (RMW, CALL, vite disgiunte, IDIV).
 * PASS 5-6: aggiunti — SETcc end-to-end e CFG multi-blocco a diamante,
 * casi non coperti in precedenza (solo blocco singolo testato).
 *
 * NOTA (block.h): BasicBlock.start/.end sono ora dentro il campo nominato
 * 'range' (BlockRange), non piu' promossi direttamente su BasicBlock.
 * Ogni literal qui sotto usa quindi .range = { .start = ..., .end = ... }
 * invece di .start/.end diretti; .succ resta un campo diretto di BasicBlock.
 */

#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "../arena.h"
#include "../liveness.h"
#include "../interference.h"

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

    /* ================================================================
     * PASS 1 (regressione Bug 1): v0 += v1 (ADD, RMW) deve produrre
     * un'interferenza v0-v1, perche' v0 e v1 sono entrambi vivi appena
     * prima dell'ADD (v0 viene letto E scritto).
     *
     *   i0: v0 = 10
     *   i1: v1 = 20
     *   i2: v0 = v0 + v1     (RMW)
     *   i3: rax = v0
     *   i4: ret
     * ================================================================ */
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
        LivenessResult liv = liveness_computeMach(&f, blocks, 1, arena);
        IGraph g = ig_build(&f, blocks, 1, f.nextVreg, liv.liveAfter, f.nextVreg, arena);

        assert(has_edge(&g, 0, 1) &&
               "v0 e v1 devono interferire: v0 e' letto E scritto dall'ADD RMW "
               "(regressione Bug1 in instr_uses)");

        printf("PASS 1 ok: ADD (RMW) genera correttamente l'arco v0-v1 (regressione Bug1).\n");
        ig_free(&g);
        arena_destroy(arena);
    }

    /* ================================================================
     * PASS 2: v0 vivo attraverso una CALL deve avere crossesCall=1 ed
     * excl[] con tutti i bit caller-saved settati.
     *
     *   i0: v0 = 1
     *   i1: call f
     *   i2: rax = v0      (v0 letto dopo la call: vivo attraverso di essa)
     *   i3: ret
     * ================================================================ */
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
        LivenessResult liv = liveness_computeMach(&f, blocks, 1, arena);
        IGraph g = ig_build(&f, blocks, 1, f.nextVreg, liv.liveAfter, f.nextVreg, arena);

        assert(g.crossesCall[0] == 1);
        uint32_t callerMask = (1U << PHYS_CALLER_SAVED_COUNT) - 1;
        assert((g.excl[0] & callerMask) == callerMask &&
               "v0 vivo attraverso la CALL deve escludere tutti i caller-saved");

        printf("PASS 2 ok: vreg vivo attraverso CALL ottiene crossesCall=1 ed excl corretto.\n");
        ig_free(&g);
        arena_destroy(arena);
    }

    /* ================================================================
     * PASS 3: due vreg con range di vita disgiunti NON devono interferire.
     *
     *   i0: v0 = 1
     *   i1: rax = v0      (v0 muore qui)
     *   i2: v1 = 2
     *   i3: rax = v1
     *   i4: ret
     * ================================================================ */
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
        LivenessResult liv = liveness_computeMach(&f, blocks, 1, arena);
        IGraph g = ig_build(&f, blocks, 1, f.nextVreg, liv.liveAfter, f.nextVreg, arena);

        assert(!has_edge(&g, 0, 1) &&
               "v0 e v1 non sono mai vivi simultaneamente: nessuna interferenza attesa");

        printf("PASS 3 ok: vite disgiunte non generano interferenza spuria.\n");
        ig_free(&g);
        arena_destroy(arena);
    }

    /* ================================================================
     * PASS 4: IDIV esclude RAX/RDX per un vreg vivo attraverso di esso.
     * v0 deve restare vivo DOPO la IDIV (letto da un'istruzione successiva)
     * perche' l'exclusion venga applicata su liveAfter[IDIV].
     *
     *   i0: v0 = 7
     *   i1: idiv v0         (legge v0 come dividendo, non lo ridefinisce)
     *   i2: rax = v0        (v0 ancora vivo dopo la IDIV)
     *   i3: ret
     * ================================================================ */
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
        LivenessResult liv = liveness_computeMach(&f, blocks, 1, arena);
        IGraph g = ig_build(&f, blocks, 1, f.nextVreg, liv.liveAfter, f.nextVreg, arena);

        uint32_t mask = (1U << PHYS_RAX) | (1U << PHYS_RDX);
        assert((g.excl[0] & mask) == mask);
        printf("PASS 4 ok: IDIV esclude RAX/RDX per il vreg coinvolto.\n");
        ig_free(&g);
        arena_destroy(arena);
    }

    /* ================================================================
     * PASS 5: SETcc scrive %al (alias di RAX). Un vreg vivo attraverso
     * una SETE deve escludere RAX senza pero' ottenere crossesCall=1
     * (riservato a CALL, non a SETcc).
     *
     *   i0: v0 = 1
     *   i1: sete %al
     *   i2: rax = v0        (v0 vivo attraverso la SETE)
     *   i3: ret
     * ================================================================ */
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
        LivenessResult liv = liveness_computeMach(&f, blocks, 1, arena);
        IGraph g = ig_build(&f, blocks, 1, f.nextVreg, liv.liveAfter, f.nextVreg, arena);

        uint32_t raxMask = (1U << PHYS_RAX);
        assert((g.excl[0] & raxMask) == raxMask &&
               "v0 vivo attraverso SETcc deve escludere RAX (alias di %al)");
        assert(g.crossesCall[0] == 0 &&
               "SETcc non e' una CALL: crossesCall non deve essere settato");

        printf("PASS 5 ok: SETcc esclude RAX per il vreg vivo attraverso di essa, senza crossesCall.\n");
        ig_free(&g);
        arena_destroy(arena);
    }

    /* ================================================================
     * PASS 6: CFG a diamante (if/else) multi-blocco — nessun test
     * precedente copriva piu' di un blocco. v0 e' definito nel blocco 0
     * e usato in ENTRAMBI i rami: deve interferire con v1 (def. nel
     * ramo then) e con v2 (def. nel ramo else); v1 e v2, appartenendo a
     * rami mutuamente esclusivi, non devono MAI interferire tra loro.
     *
     *   b0 [0,2): v0 = 10 ; je .L1              succ = { b1, b2 }
     *   b1 [2,4): v1 = v0 + 1 ; jmp .L2         succ = { b3 }
     *   b2 [4,6): v2 = v0 + 2   (LABEL .L1 apre il blocco)  succ = { b3 }
     *   b3 [6,8): (LABEL .L2) ; ret
     * ================================================================ */
    {
        MachInstr instrs[8] = {
            { .op = MACH_MOV,   .dst = vreg(0), .src1 = imm(10), .src2 = mo_none() },      /* 0 */
            { .op = MACH_JE,    .dst = label(1), .src1 = mo_none(), .src2 = mo_none() },    /* 1 */
            { .op = MACH_ADD,   .dst = vreg(1), .src1 = vreg(0), .src2 = imm(1) },          /* 2 */
            { .op = MACH_JMP,   .dst = label(2), .src1 = mo_none(), .src2 = mo_none() },    /* 3 */
            { .op = MACH_LABEL, .dst = label(1), .src1 = mo_none(), .src2 = mo_none() },    /* 4 */
            { .op = MACH_ADD,   .dst = vreg(2), .src1 = vreg(0), .src2 = imm(2) },          /* 5 */
            { .op = MACH_LABEL, .dst = label(2), .src1 = mo_none(), .src2 = mo_none() },    /* 6 */
            { .op = MACH_RET,   .dst = mo_none(), .src1 = mo_none(), .src2 = mo_none() },   /* 7 */
        };
        MachFunction f = { .name = "t6", .instrs = instrs, .count = 8,
                           .capacity = 8, .frameSize = 0, .nextVreg = 3 };

        /* Indici di BasicBlock.succ[] sono indici di BLOCCO (0..nBlocks-1),
         * non indici di istruzione — coerente con il modello usato da
         * loop.c/liveness.c. */
        BasicBlock blocks[4] = {
            { .range = { .start = 0, .end = 2 }, .succ = { 1, 2 } },  /* JE: fallthrough=b1, taken(.L1)=b2 */
            { .range = { .start = 2, .end = 4 }, .succ = { 3, -1 } }, /* JMP .L2 -> b3 */
            { .range = { .start = 4, .end = 6 }, .succ = { 3, -1 } }, /* fallthrough -> b3 */
            { .range = { .start = 6, .end = 8 }, .succ = { -1, -1 } },
        };

        Arena *arena = arena_create(0);
        LivenessResult liv = liveness_computeMach(&f, blocks, 4, arena);
        IGraph g = ig_build(&f, blocks, 4, f.nextVreg, liv.liveAfter, f.nextVreg, arena);

        assert(has_edge(&g, 0, 1) && "v0 vivo alla definizione di v1 nel ramo then");
        assert(has_edge(&g, 0, 2) && "v0 vivo alla definizione di v2 nel ramo else");
        assert(!has_edge(&g, 1, 2) &&
               "v1 e v2 appartengono a rami mutuamente esclusivi: mai vivi insieme");

        printf("PASS 6 ok: CFG a diamante, liveness cross-block corretta (join point rispettato).\n");
        ig_free(&g);
        arena_destroy(arena);
    }

    printf("\nTutti i test interference sono passati.\n");
    return 0;
}