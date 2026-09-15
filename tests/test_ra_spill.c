#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../instr_selector.h"
#include "../ra_spill.h"
#include "../reg_class.h"

/* */
static MachOperand vreg(int id)  { MachOperand o = {0}; o.kind = MO_VREG; o.vregId = id; return o; }
static MachOperand imm(long v)   { MachOperand o = {0}; o.kind = MO_IMM;  o.imm = v;      return o; }
static MachOperand mo_none(void) { MachOperand o = {0}; o.kind = MO_NONE;                 return o; }
static MachOperand mem(int base, int idx) {
    MachOperand o = {0};
    o.kind = MO_MEM;
    o.mem.baseVreg = base; o.mem.indexVreg = idx; o.mem.scale = 8; o.mem.disp = 0;
    return o;
}

/* */
static MachFunction make_func(MachInstr *src, int n, int nextVreg) {
    MachFunction f = {0};
    f.name     = "t";
    f.count    = n;
    f.capacity = n;
    f.instrs   = malloc((size_t)n * sizeof(MachInstr));
    memcpy(f.instrs, src, (size_t)n * sizeof(MachInstr));
    f.nextVreg = nextVreg;
    f.frameSize = 0;
    return f;
}

int main(void) {

    /* PASS 1 */
    {
        MachInstr src[2] = {
            { .op = MACH_ADD, .dst = vreg(1), .src1 = vreg(0), .src2 = vreg(0) },
            { .op = MACH_RET, .dst = mo_none(), .src1 = mo_none(), .src2 = mo_none() },
        };
        MachFunction f = make_func(src, 2, /* */2);
        int spilled[1] = { 0 };
        int frameOff = 0;

        ra_spill_insert(&f, RC_INT, spilled, 1, &frameOff);

        /* */
        assert(f.count == 3 && "un solo reload atteso (cache hit sul secondo uso)");
        assert(f.instrs[0].op == MACH_MOV && f.instrs[0].src1.kind == MO_STACK);
        int t = f.instrs[0].dst.vregId;
        assert(f.instrs[1].op == MACH_ADD);
        assert(f.instrs[1].src1.kind == MO_VREG && f.instrs[1].src1.vregId == t);
        assert(f.instrs[1].src2.kind == MO_VREG && f.instrs[1].src2.vregId == t &&
               "il secondo uso di v0 must reuse lo stesso reload temp (cache hit)");
        assert(f.instrs[2].op == MACH_RET);

        printf("PASS 1 ok: double use of the same spilled value in the same instruction -> one solo reload.\n");
        free(f.instrs);
    }

    /* PASS 2 */
    {
        MachInstr src[2] = {
            { .op = MACH_ADD, .dst = vreg(0), .src1 = vreg(0), .src2 = vreg(1) },
            { .op = MACH_RET, .dst = mo_none(), .src1 = mo_none(), .src2 = mo_none() },
        };
        MachFunction f = make_func(src, 2, /* */2);
        int spilled[1] = { 0 };
        int frameOff = 0;

        ra_spill_insert(&f, RC_INT, spilled, 1, &frameOff);

        /* */
        assert(f.count == 4 && "RMW spilled dst: reload + store around the instruction");
        assert(f.instrs[0].op == MACH_MOV && f.instrs[0].src1.kind == MO_STACK);
        int t = f.instrs[0].dst.vregId;

        assert(f.instrs[1].op == MACH_ADD);
        assert(f.instrs[1].dst.vregId  == t && "dst riscritto sul reload temp");
        assert(f.instrs[1].src1.vregId == t && "src1 (era v0) must point allo stesso reload (RMW)");
        assert(f.instrs[1].src2.kind == MO_VREG && f.instrs[1].src2.vregId == 1 &&
               "v1 non spillato: invariato");

        assert(f.instrs[2].op == MACH_MOV);
        assert(f.instrs[2].dst.kind == MO_STACK && "store back nello slot di v0");
        assert(f.instrs[2].src1.kind == MO_VREG && f.instrs[2].src1.vregId == t);

        assert(f.instrs[3].op == MACH_RET);
        assert(frameOff == 8);

        printf("PASS 2 ok: dst spillato RMW -> reload prima, store dopo, stesso temp riusato.\n");
        free(f.instrs);
    }

    /* PASS 3 */
    {
        MachInstr src[2] = {
            { .op = MACH_MOV, .dst = vreg(0), .src1 = imm(7), .src2 = mo_none() },
            { .op = MACH_RET, .dst = mo_none(), .src1 = mo_none(), .src2 = mo_none() },
        };
        MachFunction f = make_func(src, 2, /* */1);
        int spilled[1] = { 0 };
        int frameOff = 0;

        ra_spill_insert(&f, RC_INT, spilled, 1, &frameOff);

        /* */
        assert(f.count == 3 && "pura scrittura: no reload, solo store");
        assert(f.instrs[0].op == MACH_MOV && f.instrs[0].src1.kind == MO_IMM &&
               f.instrs[0].src1.imm == 7);
        int fresh = f.instrs[0].dst.vregId;
        assert(f.instrs[1].op == MACH_MOV && f.instrs[1].dst.kind == MO_STACK);
        assert(f.instrs[1].src1.vregId == fresh);
        assert(f.instrs[2].op == MACH_RET);

        printf("PASS 3 ok: dst spillato con scrittura pura -> no reload, solo store.\n");
        free(f.instrs);
    }

    /* PASS 4 */
    {
        MachInstr src[3] = {
            { .op = MACH_ADD, .dst = vreg(2), .src1 = vreg(0), .src2 = vreg(1) },
            { .op = MACH_ADD, .dst = vreg(3), .src1 = vreg(0), .src2 = vreg(1) },
            { .op = MACH_RET, .dst = mo_none(), .src1 = mo_none(), .src2 = mo_none() },
        };
        MachFunction f = make_func(src, 3, /* */4);
        int spilled[1] = { 0 };
        int frameOff = 0;

        ra_spill_insert(&f, RC_INT, spilled, 1, &frameOff);

        /* */
        assert(f.count == 5 && "two distinct reloads expected (cache does not persist across instructions)");
        assert(f.instrs[0].op == MACH_MOV && f.instrs[0].src1.kind == MO_STACK);
        int ta = f.instrs[0].dst.vregId;
        assert(f.instrs[1].op == MACH_ADD && f.instrs[1].src1.vregId == ta);

        assert(f.instrs[2].op == MACH_MOV && f.instrs[2].src1.kind == MO_STACK);
        int tb = f.instrs[2].dst.vregId;
        assert(f.instrs[3].op == MACH_ADD && f.instrs[3].src1.vregId == tb);

        assert(ta != tb && "reload of the second instruction must be a NEW temp");
        assert(f.instrs[4].op == MACH_RET);

        printf("PASS 4 ok: reload cache reset correctly between different instructions.\n");
        free(f.instrs);
    }

    /* PASS 5 */
    {
        MachInstr src[2] = {
            { .op = MACH_STORE, .dst = mem(0, 1), .src1 = vreg(2), .src2 = mo_none() },
            { .op = MACH_RET,   .dst = mo_none(), .src1 = mo_none(), .src2 = mo_none() },
        };
        MachFunction f = make_func(src, 2, /* */3);
        int spilled[2] = { 0, 1 };
        int frameOff = 0;

        ra_spill_insert(&f, RC_INT, spilled, 2, &frameOff);

        /* */
        assert(f.count == 4 && "base+index spillati: due reload prima dello STORE");
        assert(f.instrs[0].op == MACH_MOV && f.instrs[0].src1.kind == MO_STACK);
        int tbase = f.instrs[0].dst.vregId;
        assert(f.instrs[1].op == MACH_MOV && f.instrs[1].src1.kind == MO_STACK);
        int tidx = f.instrs[1].dst.vregId;
        assert(tbase != tidx);

        assert(f.instrs[2].op == MACH_STORE);
        assert(f.instrs[2].dst.kind == MO_MEM);
        assert(f.instrs[2].dst.mem.baseVreg  == tbase);
        assert(f.instrs[2].dst.mem.indexVreg == tidx);
        assert(f.instrs[2].src1.kind == MO_VREG && f.instrs[2].src1.vregId == 2 &&
               "valore da scrivere (v2) non spillato: invariato");

        assert(f.instrs[3].op == MACH_RET);
        assert(frameOff == 16 && "due slot da 8 byte assegnati");

        printf("PASS 5 ok: STORE con base+index MO_MEM entrambi spillati ricaricati correctmente.\n");
        free(f.instrs);
    }

    /* PASS 6 */
    {
        MachInstr src[2] = {
            { .op = MACH_ADD, .dst = vreg(5), .src1 = vreg(0), .src2 = imm(0) },
            { .op = MACH_RET, .dst = mo_none(), .src1 = mo_none(), .src2 = mo_none() },
        };
        MachFunction f = make_func(src, 2, /* */6);
        int spilled[3] = { 2, 0, 1 }; /* */
        int frameOff = 0;

        ra_spill_insert(&f, RC_INT, spilled, 3, &frameOff);

        assert(frameOff == 24 && "3 slot da 8 byte, incremento indipendente dall'id numerico");
        /* */
        assert(f.instrs[0].op == MACH_MOV && f.instrs[0].src1.kind == MO_STACK);
        assert(f.instrs[0].src1.stackOff == 16 &&
               "slot di v0 must be il secondo assegnato (ordine di spilled[], non id)");

        printf("PASS 6 ok: slot/frameOff assigned in spilled[] order, not by increasing id.\n");
        free(f.instrs);
    }

    printf("\nAll ra_spill tests passed.\n");
    return 0;
}
