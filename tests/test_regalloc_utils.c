/**
 * @file test_regalloc_utils.c
 * @brief Unit test per instr_query.h/.c (operand extraction, opcode
 *        predicates) e per regalloc_spill_weight() (regalloc_utils.h/.c).
 *
 * Copre in particolare la regressione del Bug 1: instr_uses() deve contare
 * dst come use per gli opcode RMW (ADD/SUB/IMUL/SAL/NEG/NOT/XOR), non solo
 * per STORE/PUSH/IDIV/CQO.
 *
 * NOTA: le funzioni di query erano originariamente in regalloc_utils.h
 * (regalloc_is_rmw/regalloc_is_setcc/regalloc_is_ctrl_transfer). Sono state
 * spostate in instr_query.h e rinominate instr_is_rmw/instr_is_setcc/
 * instr_is_ctrl_transfer per rimuovere una dipendenza a rovescio: lo
 * scheduler (fase precedente al regalloc nella pipeline) usava funzioni
 * definite in un modulo con nome/scopo "regalloc". regalloc_utils.h ora
 * contiene solo regalloc_spill_weight(), genuinamente specifico
 * dell'allocatore di registri.
 */

#include <stdio.h>
#include <assert.h>
#include "../instr_selector.h"
#include "../instr_query.h"
#include "../regalloc_utils.h"

/* Helper: costruisce un MachOperand MO_VREG. */
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

/* Cerca id in un array out[0..n) */
static int contains(const int *out, int n, int id) {
    for (int i = 0; i < n; i++) if (out[i] == id) return 1;
    return 0;
}

int main(void) {
    int buf[16], n;

    /* ---- PASS 1 (regressione Bug 1): ADD e' RMW, dst deve essere un uso ---- */
    {
        MachInstr add = { .op = MACH_ADD, .dst = vreg(0), .src1 = vreg(1), .src2 = mo_none() };
        instr_uses(&add, /*nextVreg=*/100, buf, &n);
        assert(contains(buf, n, 0) && "dst (v0) deve comparire tra gli usi di un ADD RMW");
        assert(contains(buf, n, 1) && "src1 (v1) deve comparire tra gli usi");
        printf("PASS 1 ok: instr_uses(ADD) include dst come RMW-use (regressione Bug1).\n");
    }

    /* PASS 1-bis: stessa verifica per tutta la famiglia RMW */
    {
        MachOpCode rmwOps[] = { MACH_SUB, MACH_IMUL, MACH_SAL, MACH_NEG, MACH_NOT, MACH_XOR };
        for (size_t i = 0; i < sizeof(rmwOps)/sizeof(rmwOps[0]); i++) {
            MachInstr in = { .op = rmwOps[i], .dst = vreg(7), .src1 = vreg(8), .src2 = mo_none() };
            instr_uses(&in, 100, buf, &n);
            assert(contains(buf, n, 7));
            assert(instr_is_rmw(rmwOps[i]));
        }
        printf("PASS 1bis ok: tutta la famiglia RMW conta dst come use.\n");
    }

    /* ---- PASS 2: MOV non e' RMW, dst NON deve comparire negli usi ---- */
    {
        MachInstr mov = { .op = MACH_MOV, .dst = vreg(2), .src1 = vreg(3), .src2 = mo_none() };
        instr_uses(&mov, 100, buf, &n);
        assert(!contains(buf, n, 2) && "MOV non e' RMW: dst non deve essere un uso");
        assert(contains(buf, n, 3));
        assert(!instr_is_rmw(MACH_MOV));
        printf("PASS 2 ok: MOV non tratta dst come use (nessun falso positivo).\n");
    }

    /* ---- PASS 3: STORE legge base+index del MO_MEM in dst ---- */
    {
        MachInstr st = { .op = MACH_STORE, .dst = mem(4, 5), .src1 = vreg(6), .src2 = mo_none() };
        instr_uses(&st, 100, buf, &n);
        assert(contains(buf, n, 4) && "base register della destinazione MO_MEM");
        assert(contains(buf, n, 5) && "index register della destinazione MO_MEM");
        assert(contains(buf, n, 6) && "valore sorgente scritto in memoria");
        printf("PASS 3 ok: STORE conta base+index del MO_MEM come usi.\n");
    }

    /* ---- PASS 4: IDIV / CQO leggono dst (dividendo) ---- */
    {
        MachInstr idiv = { .op = MACH_IDIV, .dst = vreg(9), .src1 = mo_none(), .src2 = mo_none() };
        instr_uses(&idiv, 100, buf, &n);
        assert(contains(buf, n, 9));

        MachInstr cqo = { .op = MACH_CQO, .dst = vreg(11), .src1 = mo_none(), .src2 = mo_none() };
        instr_uses(&cqo, 100, buf, &n);
        assert(contains(buf, n, 11));
        printf("PASS 4 ok: IDIV/CQO leggono dst come dividendo/sorgente sign-extend.\n");
    }

    /* ---- PASS 5: instr_defs / instr_def -- CMP/TEST/branch non definiscono nulla ---- */
    {
        MachInstr cmp = { .op = MACH_CMP, .dst = vreg(1), .src1 = vreg(2), .src2 = mo_none() };
        assert(instr_def(&cmp, 100) == -1);
        instr_defs(&cmp, 100, buf, &n);
        assert(n == 0);

        MachInstr add = { .op = MACH_ADD, .dst = vreg(3), .src1 = vreg(4), .src2 = mo_none() };
        instr_defs(&add, 100, buf, &n);
        assert(n == 1 && buf[0] == 3);
        printf("PASS 5 ok: instr_defs corretto per opcode senza/con destinazione.\n");
    }

    /* ---- PASS 6: implicit uses/defs per CALL (tutti i caller-saved) ---- */
    {
        MachInstr call = { .op = MACH_CALL, .dst = mo_none(), .src1 = mo_none(), .src2 = mo_none() };
        int nextVreg = 50;
        instr_implicit_uses(&call, nextVreg, buf, &n);
        assert(n == PHYS_CALLER_SAVED_COUNT);
        for (int p = 0; p < PHYS_CALLER_SAVED_COUNT; p++)
            assert(contains(buf, n, nextVreg + p));

        instr_implicit_defs(&call, nextVreg, buf, &n);
        assert(n == PHYS_CALLER_SAVED_COUNT);
        printf("PASS 6 ok: CALL usa/clobbera tutti i registri caller-saved.\n");
    }

    /* ---- PASS 7: IDIV implicit uses/defs = RAX+RDX ---- */
    {
        MachInstr idiv = { .op = MACH_IDIV, .dst = vreg(0), .src1 = mo_none(), .src2 = mo_none() };
        int nextVreg = 20;
        instr_implicit_uses(&idiv, nextVreg, buf, &n);
        assert(n == 2 && contains(buf, n, nextVreg + PHYS_RAX) && contains(buf, n, nextVreg + PHYS_RDX));
        instr_implicit_defs(&idiv, nextVreg, buf, &n);
        assert(n == 2 && contains(buf, n, nextVreg + PHYS_RAX) && contains(buf, n, nextVreg + PHYS_RDX));
        printf("PASS 7 ok: IDIV legge/scrive implicitamente RAX:RDX.\n");
    }

    /* ---- PASS 8: predicati ausiliari ---- */
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
        assert(regalloc_spill_weight(99) == regalloc_spill_weight(5)); /* clamp */
        printf("PASS 8 ok: predicati setcc/ctrl-transfer (instr_query) e spill weight (regalloc_utils) corretti.\n");
    }

    /* ---- PASS 9: PHYS_AL normalizzato a PHYS_RAX nelle letture esplicite ---- */
    {
        MachInstr push = { .op = MACH_PUSH, .dst = phys(PHYS_AL), .src1 = mo_none(), .src2 = mo_none() };
        int nextVreg = 100;
        instr_uses(&push, nextVreg, buf, &n);
        assert(contains(buf, n, nextVreg + PHYS_RAX) && "PHYS_AL deve essere normalizzato a PHYS_RAX (offset da nextVreg)");
        assert(!contains(buf, n, nextVreg + PHYS_AL));
        printf("PASS 9 ok: PHYS_AL normalizzato a PHYS_RAX.\n");
    }

    /* ---- PASS 10: imm come src non produce alcun id ---- */
    {
        MachInstr add = { .op = MACH_ADD, .dst = vreg(0), .src1 = imm(42), .src2 = mo_none() };
        instr_uses(&add, 100, buf, &n);
        /* solo dst (RMW use), l'immediato non ha id */
        assert(n == 1 && buf[0] == 0);
        printf("PASS 10 ok: operando immediato non produce alcun id di registro.\n");
    }

    printf("\nTutti i test instr_query/regalloc_utils sono passati.\n");
    return 0;
}