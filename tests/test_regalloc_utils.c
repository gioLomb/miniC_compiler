#include <stdio.h>
#include <assert.h>
#include "../instr_selector.h"
#include "../instr_query.h"
#include "../reg_class.h"
#include "../regalloc_utils.h"

static MachOperand vreg(int id) {
    MachOperand o = {0}; o.kind = MO_VREG; o.vregId = id; return o;
}
static MachOperand phys(int p) {
    MachOperand o = {0}; o.kind = MO_PHYS; o.physReg = p; return o;
}
static MachOperand imm(long v) {
    MachOperand o = {0}; o.kind = MO_IMM; o.imm = v; return o;
}
static MachOperand mo_none(void) {
    MachOperand o = {0}; o.kind = MO_NONE; return o;
}
static MachOperand mem(int base, int idx) {
    MachOperand o = {0}; o.kind = MO_MEM;
    o.mem.baseVreg = base; o.mem.indexVreg = idx; o.mem.scale = 8; o.mem.disp = 0;
    return o;
}

static int contains(const int *out, int n, int id) {
    for (int i = 0; i < n; i++) if (out[i] == id) return 1;
    return 0;
}

int main(void) {
    int buf[16], n;

    /* ADD is RMW: dst must appear among the uses */
    {
        MachInstr add = { .op = MACH_ADD, .dst = vreg(0), .src1 = vreg(1), .src2 = mo_none() };
        instr_uses(&add, /*nextVreg=*/100, RC_INT, buf, &n);
        assert(contains(buf, n, 0) && "dst (v0) must appear among uses of an RMW ADD");
        assert(contains(buf, n, 1) && "src1 (v1) must appear among uses");
        printf("PASS 1 ok: instr_uses(ADD) includes dst as RMW-use.\n");
    }

    /* Same check for the whole RMW family */
    {
        MachOpCode rmwOps[] = { MACH_SUB, MACH_IMUL, MACH_SAL, MACH_NEG, MACH_NOT, MACH_XOR };
        for (size_t i = 0; i < sizeof(rmwOps)/sizeof(rmwOps[0]); i++) {
            MachInstr in = { .op = rmwOps[i], .dst = vreg(7), .src1 = vreg(8), .src2 = mo_none() };
            instr_uses(&in, 100, RC_INT, buf, &n);
            assert(contains(buf, n, 7));
            assert(instr_is_rmw(rmwOps[i]));
        }
        printf("PASS 1bis ok: entire RMW family counts dst as use.\n");
    }

    /* MOV is not RMW: dst must not appear among uses */
    {
        MachInstr mov = { .op = MACH_MOV, .dst = vreg(2), .src1 = vreg(3), .src2 = mo_none() };
        instr_uses(&mov, 100, RC_INT, buf, &n);
        assert(!contains(buf, n, 2) && "MOV is not RMW: dst must not be a use");
        assert(contains(buf, n, 3));
        assert(!instr_is_rmw(MACH_MOV));
        printf("PASS 2 ok: MOV does not treat dst as use.\n");
    }

    /* STORE reads base+index of the MO_MEM in dst */
    {
        MachInstr st = { .op = MACH_STORE, .dst = mem(4, 5), .src1 = vreg(6), .src2 = mo_none() };
        instr_uses(&st, 100, RC_INT, buf, &n);
        assert(contains(buf, n, 4) && "base register of destination MO_MEM");
        assert(contains(buf, n, 5) && "index register of destination MO_MEM");
        assert(contains(buf, n, 6) && "source value written to memory");
        printf("PASS 3 ok: STORE counts base+index of MO_MEM as uses.\n");
    }

    /* IDIV / CQO read dst (dividend / sign-extend source) */
    {
        MachInstr idiv = { .op = MACH_IDIV, .dst = vreg(9), .src1 = mo_none(), .src2 = mo_none() };
        instr_uses(&idiv, 100, RC_INT, buf, &n);
        assert(contains(buf, n, 9));

        MachInstr cqo = { .op = MACH_CQO, .dst = vreg(11), .src1 = mo_none(), .src2 = mo_none() };
        instr_uses(&cqo, 100, RC_INT, buf, &n);
        assert(contains(buf, n, 11));
        printf("PASS 4 ok: IDIV/CQO read dst as dividend/sign-extend source.\n");
    }

    /* CMP/TEST/branch define nothing; ADD defines dst */
    {
        MachInstr cmp = { .op = MACH_CMP, .dst = vreg(1), .src1 = vreg(2), .src2 = mo_none() };
        assert(instr_def(&cmp, 100, RC_INT) == -1);
        instr_defs(&cmp, 100, RC_INT, buf, &n);
        assert(n == 0);

        MachInstr add = { .op = MACH_ADD, .dst = vreg(3), .src1 = vreg(4), .src2 = mo_none() };
        instr_defs(&add, 100, RC_INT, buf, &n);
        assert(n == 1 && buf[0] == 3);
        printf("PASS 5 ok: instr_defs correct for opcodes with/without destination.\n");
    }

    /* CALL implicit uses/defs = all caller-saved */
    {
        MachInstr call = { .op = MACH_CALL, .dst = mo_none(), .src1 = mo_none(), .src2 = mo_none() };
        int nextVreg = 50;
        instr_implicit_uses(&call, nextVreg, RC_INT, buf, &n);
        assert(n == PHYS_CALLER_SAVED_COUNT);
        for (int p = 0; p < PHYS_CALLER_SAVED_COUNT; p++)
            assert(contains(buf, n, nextVreg + p));

        instr_implicit_defs(&call, nextVreg, RC_INT, buf, &n);
        assert(n == PHYS_CALLER_SAVED_COUNT);
        printf("PASS 6 ok: CALL uses/clobbers all caller-saved registers.\n");
    }

    /* IDIV implicit uses/defs = RAX+RDX */
    {
        MachInstr idiv = { .op = MACH_IDIV, .dst = vreg(0), .src1 = mo_none(), .src2 = mo_none() };
        int nextVreg = 20;
        instr_implicit_uses(&idiv, nextVreg, RC_INT, buf, &n);
        assert(n == 2 && contains(buf, n, nextVreg + PHYS_RAX) && contains(buf, n, nextVreg + PHYS_RDX));
        instr_implicit_defs(&idiv, nextVreg, RC_INT, buf, &n);
        assert(n == 2 && contains(buf, n, nextVreg + PHYS_RAX) && contains(buf, n, nextVreg + PHYS_RDX));
        printf("PASS 7 ok: IDIV reads/writes RAX:RDX implicitly.\n");
    }

    /* Auxiliary predicates */
    {
        assert(instr_is_setcc(MACH_SETE));
        assert(instr_is_setcc(MACH_SETGE));
        assert(!instr_is_setcc(MACH_MOV));

        assert(instr_is_ctrl_transfer(MACH_JMP));
        assert(instr_is_ctrl_transfer(MACH_CALL));
        assert(instr_is_ctrl_transfer(MACH_RET));
        assert(!instr_is_ctrl_transfer(MACH_ADD));

        assert(regalloc_spill_weight(0) == 1);
        assert(regalloc_spill_weight(2) == 100);
        assert(regalloc_spill_weight(99) == regalloc_spill_weight(5));
        printf("PASS 8 ok: setcc/ctrl-transfer predicates and spill weight correct.\n");
    }

    /* PHYS_AL normalized to PHYS_RAX in explicit reads */
    {
        MachInstr push = { .op = MACH_PUSH, .dst = phys(PHYS_AL), .src1 = mo_none(), .src2 = mo_none() };
        int nextVreg = 100;
        instr_uses(&push, nextVreg, RC_INT, buf, &n);
        assert(contains(buf, n, nextVreg + PHYS_RAX) && "PHYS_AL must be normalized to PHYS_RAX");
        assert(!contains(buf, n, nextVreg + PHYS_AL));
        printf("PASS 9 ok: PHYS_AL normalized to PHYS_RAX.\n");
    }

    /* Immediate src produces no register id */
    {
        MachInstr add = { .op = MACH_ADD, .dst = vreg(0), .src1 = imm(42), .src2 = mo_none() };
        instr_uses(&add, 100, RC_INT, buf, &n);
        assert(n == 1 && buf[0] == 0);
        printf("PASS 10 ok: immediate operand produces no register id.\n");
    }

    printf("\nAll instr_query/regalloc_utils tests passed.\n");
    return 0;
}
