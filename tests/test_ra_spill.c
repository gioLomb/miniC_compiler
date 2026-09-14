/**
 * @file test_ra_spill.c
 * @brief Unit test per ra_spill.c (inserimento codice di spill).
 *
 * Nessun test diretto esisteva per questo modulo prima d'ora: ra_spill.c
 * e' esercitato solo indirettamente tramite regalloc_function() quando
 * ra_select_colors() produce spilled[] non vuoto. Qui si chiama
 * ra_spill_insert() direttamente su MachFunction sintetiche per isolare:
 *
 *   PASS 1 - cache hit all'interno della STESSA istruzione (stesso vreg
 *            spillato usato sia come src1 che src2): un solo reload.
 *   PASS 2 - dst spillato con opcode RMW (ADD): reload obbligatorio PRIMA
 *            dell'istruzione (il valore vecchio viene letto), poi store
 *            del risultato dopo.
 *   PASS 3 - dst spillato con opcode NON RMW (MOV, pura scrittura): nessun
 *            reload, solo store dopo.
 *   PASS 4 - la cache NON deve persistere tra istruzioni diverse (vedi
 *            nota di correttezza in ra_spill.c: altrimenti il loop di
 *            regalloc_function non converge). Due istruzioni consecutive
 *            che leggono lo stesso vreg spillato devono generare due
 *            reload distinti (temp diversi).
 *   PASS 5 - reload di base E index di un operando MO_MEM (STORE).
 *   PASS 6 - crescita di *frameOff e assegnazione slot nell'ordine in cui
 *            compaiono in spilled[], non nell'ordine numerico dei vreg id.
 *
 * ra_spill_insert() chiama free(f->instrs): l'array di input deve quindi
 * essere malloc'ato (mai un array automatico), altrimenti free() su
 * memoria non heap e' undefined behaviour.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../instr_selector.h"
#include "../ra_spill.h"
#include "../reg_class.h"

/* ---- Operand builders (stesso stile di test_interference.c) ---- */
static MachOperand vreg(int id)  { MachOperand o = {0}; o.kind = MO_VREG; o.vregId = id; return o; }
static MachOperand imm(long v)   { MachOperand o = {0}; o.kind = MO_IMM;  o.imm = v;      return o; }
static MachOperand mo_none(void) { MachOperand o = {0}; o.kind = MO_NONE;                 return o; }
static MachOperand mem(int base, int idx) {
    MachOperand o = {0};
    o.kind = MO_MEM;
    o.mem.baseVreg = base; o.mem.indexVreg = idx; o.mem.scale = 8; o.mem.disp = 0;
    return o;
}

/* Costruisce una MachFunction con instrs malloc'ata (ra_spill_insert la libera). */
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

    /* ================================================================
     * PASS 1: v0 spillato, usato due volte nella STESSA istruzione
     * (src1 e src2). Deve produrre UN SOLO reload (cache hit interno).
     *
     *   i0: v1 = v0 + v0     (v0 spillato)
     *   i1: ret
     * ================================================================ */
    {
        MachInstr src[2] = {
            { .op = MACH_ADD, .dst = vreg(1), .src1 = vreg(0), .src2 = vreg(0) },
            { .op = MACH_RET, .dst = mo_none(), .src1 = mo_none(), .src2 = mo_none() },
        };
        MachFunction f = make_func(src, 2, /*nextVreg=*/2);
        int spilled[1] = { 0 };
        int frameOff = 0;

        ra_spill_insert(&f, RC_INT, spilled, 1, &frameOff);

        /* atteso: [load t<-slot, ADD v1=t+t, RET] */
        assert(f.count == 3 && "un solo reload atteso (cache hit sul secondo uso)");
        assert(f.instrs[0].op == MACH_MOV && f.instrs[0].src1.kind == MO_STACK);
        int t = f.instrs[0].dst.vregId;
        assert(f.instrs[1].op == MACH_ADD);
        assert(f.instrs[1].src1.kind == MO_VREG && f.instrs[1].src1.vregId == t);
        assert(f.instrs[1].src2.kind == MO_VREG && f.instrs[1].src2.vregId == t &&
               "il secondo uso di v0 deve riusare lo stesso reload temp (cache hit)");
        assert(f.instrs[2].op == MACH_RET);

        printf("PASS 1 ok: doppio uso dello stesso spilled nella stessa istruzione -> un solo reload.\n");
        free(f.instrs);
    }

    /* ================================================================
     * PASS 2: dst spillato, opcode RMW (ADD) -> reload PRIMA (il valore
     * corrente di v0 e' letto dall'ADD), poi store del risultato dopo.
     *
     *   i0: v0 = v0 + v1     (v0 spillato, RMW, v1 NON spillato)
     *   i1: ret
     * ================================================================ */
    {
        MachInstr src[2] = {
            { .op = MACH_ADD, .dst = vreg(0), .src1 = vreg(0), .src2 = vreg(1) },
            { .op = MACH_RET, .dst = mo_none(), .src1 = mo_none(), .src2 = mo_none() },
        };
        MachFunction f = make_func(src, 2, /*nextVreg=*/2);
        int spilled[1] = { 0 };
        int frameOff = 0;

        ra_spill_insert(&f, RC_INT, spilled, 1, &frameOff);

        /* atteso: [load t<-slot(v0), ADD t=t+v1, store slot(v0)<-t, RET] */
        assert(f.count == 4 && "RMW dst spillato: reload + store attorno all'istruzione");
        assert(f.instrs[0].op == MACH_MOV && f.instrs[0].src1.kind == MO_STACK);
        int t = f.instrs[0].dst.vregId;

        assert(f.instrs[1].op == MACH_ADD);
        assert(f.instrs[1].dst.vregId  == t && "dst riscritto sul reload temp");
        assert(f.instrs[1].src1.vregId == t && "src1 (era v0) deve puntare allo stesso reload (RMW)");
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

    /* ================================================================
     * PASS 3: dst spillato, opcode NON RMW (MOV, pura scrittura) ->
     * nessun reload, solo store dopo con un temp fresco.
     *
     *   i0: v0 = 7           (v0 spillato, MOV non e' RMW)
     *   i1: ret
     * ================================================================ */
    {
        MachInstr src[2] = {
            { .op = MACH_MOV, .dst = vreg(0), .src1 = imm(7), .src2 = mo_none() },
            { .op = MACH_RET, .dst = mo_none(), .src1 = mo_none(), .src2 = mo_none() },
        };
        MachFunction f = make_func(src, 2, /*nextVreg=*/1);
        int spilled[1] = { 0 };
        int frameOff = 0;

        ra_spill_insert(&f, RC_INT, spilled, 1, &frameOff);

        /* atteso: [MOV fresh<-7, store slot(v0)<-fresh, RET] -- NESSUN reload */
        assert(f.count == 3 && "pura scrittura: nessun reload, solo store");
        assert(f.instrs[0].op == MACH_MOV && f.instrs[0].src1.kind == MO_IMM &&
               f.instrs[0].src1.imm == 7);
        int fresh = f.instrs[0].dst.vregId;
        assert(f.instrs[1].op == MACH_MOV && f.instrs[1].dst.kind == MO_STACK);
        assert(f.instrs[1].src1.vregId == fresh);
        assert(f.instrs[2].op == MACH_RET);

        printf("PASS 3 ok: dst spillato con scrittura pura -> nessun reload, solo store.\n");
        free(f.instrs);
    }

    /* ================================================================
     * PASS 4 (regressione correttezza documentata in ra_spill.c): la
     * cache NON deve persistere tra istruzioni diverse. Due ADD separate
     * che leggono lo stesso v0 spillato devono ricevere reload DISTINTI.
     *
     *   i0: v2 = v0 + v1
     *   i1: v3 = v0 + v1
     *   i2: ret
     * ================================================================ */
    {
        MachInstr src[3] = {
            { .op = MACH_ADD, .dst = vreg(2), .src1 = vreg(0), .src2 = vreg(1) },
            { .op = MACH_ADD, .dst = vreg(3), .src1 = vreg(0), .src2 = vreg(1) },
            { .op = MACH_RET, .dst = mo_none(), .src1 = mo_none(), .src2 = mo_none() },
        };
        MachFunction f = make_func(src, 3, /*nextVreg=*/4);
        int spilled[1] = { 0 };
        int frameOff = 0;

        ra_spill_insert(&f, RC_INT, spilled, 1, &frameOff);

        /* atteso: [load ta, ADD v2=ta+v1, load tb, ADD v3=tb+v1, RET] */
        assert(f.count == 5 && "due reload distinti attesi (cache non persiste tra istruzioni)");
        assert(f.instrs[0].op == MACH_MOV && f.instrs[0].src1.kind == MO_STACK);
        int ta = f.instrs[0].dst.vregId;
        assert(f.instrs[1].op == MACH_ADD && f.instrs[1].src1.vregId == ta);

        assert(f.instrs[2].op == MACH_MOV && f.instrs[2].src1.kind == MO_STACK);
        int tb = f.instrs[2].dst.vregId;
        assert(f.instrs[3].op == MACH_ADD && f.instrs[3].src1.vregId == tb);

        assert(ta != tb && "reload della seconda istruzione deve essere un temp NUOVO");
        assert(f.instrs[4].op == MACH_RET);

        printf("PASS 4 ok: reload cache resettata correttamente tra istruzioni diverse.\n");
        free(f.instrs);
    }

    /* ================================================================
     * PASS 5: STORE con base E index di MO_MEM entrambi spillati ->
     * entrambi devono essere ricaricati prima dello STORE.
     *
     *   i0: STORE mem(base=v0, idx=v1) <- v2   (v0,v1 spillati; v2 no)
     *   i1: ret
     * ================================================================ */
    {
        MachInstr src[2] = {
            { .op = MACH_STORE, .dst = mem(0, 1), .src1 = vreg(2), .src2 = mo_none() },
            { .op = MACH_RET,   .dst = mo_none(), .src1 = mo_none(), .src2 = mo_none() },
        };
        MachFunction f = make_func(src, 2, /*nextVreg=*/3);
        int spilled[2] = { 0, 1 };
        int frameOff = 0;

        ra_spill_insert(&f, RC_INT, spilled, 2, &frameOff);

        /* atteso: [load tbase, load tidx, STORE mem(tbase,tidx)<-v2, RET] */
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

        printf("PASS 5 ok: STORE con base+index MO_MEM entrambi spillati ricaricati correttamente.\n");
        free(f.instrs);
    }

    /* ================================================================
     * PASS 6: ordine di assegnazione slot segue l'ordine di spilled[],
     * non l'ordine numerico dei vreg id. spilled = {2, 0, 1}.
     *
     *   i0: v5 = v0 + 0      (solo per verificare lo stackOff del reload di v0)
     *   i1: ret
     * ================================================================ */
    {
        MachInstr src[2] = {
            { .op = MACH_ADD, .dst = vreg(5), .src1 = vreg(0), .src2 = imm(0) },
            { .op = MACH_RET, .dst = mo_none(), .src1 = mo_none(), .src2 = mo_none() },
        };
        MachFunction f = make_func(src, 2, /*nextVreg=*/6);
        int spilled[3] = { 2, 0, 1 }; /* ordine arbitrario, v0 e' il SECONDO */
        int frameOff = 0;

        ra_spill_insert(&f, RC_INT, spilled, 3, &frameOff);

        assert(frameOff == 24 && "3 slot da 8 byte, incremento indipendente dall'id numerico");
        /* v0 e' il secondo elemento di spilled[] -> secondo increment -> offset 16 */
        assert(f.instrs[0].op == MACH_MOV && f.instrs[0].src1.kind == MO_STACK);
        assert(f.instrs[0].src1.stackOff == 16 &&
               "slot di v0 deve essere il secondo assegnato (ordine di spilled[], non id)");

        printf("PASS 6 ok: slot/frameOff assegnati nell'ordine di spilled[], non per id crescente.\n");
        free(f.instrs);
    }

    printf("\nTutti i test ra_spill sono passati.\n");
    return 0;
}
