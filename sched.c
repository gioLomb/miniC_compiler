#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "sched.h"

/* =========================================================================
 * Latenze stimate per Intel Core i5 (Haswell/Broadwell).
 * Fonte: tabelle Agner Fog, Intel Optimization Reference Manual.
 * Si usa la latenza (cicli prima che il risultato sia disponibile),
 * non il throughput reciproco, perché lo scheduler vuole anticipare
 * le istruzioni il cui risultato è richiesto prima possibile.
 * ========================================================================= */
static int latency_of(MachOp op) {
    switch (op) {
    /* Aritmetica intera leggera */
    case MACH_ADD:
    case MACH_SUB:
    case MACH_NEG:
    case MACH_NOT:
    case MACH_XOR:
        return 1;

    /* Moltiplicazione intera: 3 cicli su Haswell */
    case MACH_IMUL:
        return 3;

    /* Divisione intera: latenza molto variabile, usiamo lower bound */
    case MACH_IDIV:
        return 20;

    /* Shift */
    case MACH_SAL:
        return 1;

    /* Mov registro-registro o immediato */
    case MACH_MOV:
    case MACH_MOVSX:
        return 1;   /* reg←reg; reg←mem è gestito sotto come LOAD */

    /* Accesso memoria: ~4 cicli per L1 hit */
    case MACH_LOAD:
    case MACH_STORE:
    case MACH_PUSH:
    case MACH_POP:
        return 4;

    /* Confronto e test: 1 ciclo, producono solo FLAGS */
    case MACH_CMP:
    case MACH_TEST:
        return 1;

    /* Setcc: 1 ciclo */
    case MACH_SETE:  case MACH_SETNE:
    case MACH_SETL:  case MACH_SETLE:
    case MACH_SETG:  case MACH_SETGE:
        return 1;

    /* Salti: 1 ciclo (branch predictor gestisce il resto) */
    case MACH_JMP:
    case MACH_JE:  case MACH_JNE:
    case MACH_JL:  case MACH_JLE:
    case MACH_JG:  case MACH_JGE:
        return 1;

    /* Call/ret: ~3 cicli */
    case MACH_CALL:
    case MACH_RET:
        return 3;

    /* CQO, pseudo */
    case MACH_CQO:
        return 1;

    /* Prologo/epilogo: marcatori, non schedulabili */
    case MACH_LABEL:
    case MACH_FUNC_BEGIN:
    case MACH_FUNC_END:
        return 0;

    default:
        return 1;
    }
}

/* =========================================================================
 * Operand helpers: estrae l'ID di un virtual/physical register da un
 * MachOperand per il confronto nelle dipendenze.
 * Usiamo un intero a 32 bit: bit[30]=1 → phys, altrimenti vreg. -1 = nessuno.
 * ========================================================================= */
#define REG_NONE      (-1)
#define REG_VREG(id)  ((int)(id))
#define REG_PHYS(id)  ((int)(0x40000000 | (id)))

static int operand_reg(const MachOperand *o) {
    switch (o->kind) {
    case MO_VREG: return REG_VREG(o->vregId);
    case MO_PHYS: return REG_PHYS(o->physReg);
    case MO_MEM:
        /* base e index sono entrambi potenziali sorgenti */
        if (o->mem.baseVreg >= 0) return REG_VREG(o->mem.baseVreg);
        return REG_NONE;
    default:
        return REG_NONE;
    }
}

/* Secondo registro per MO_MEM (index), altrimenti NONE */
static int operand_reg2(const MachOperand *o) {
    if (o->kind == MO_MEM && o->mem.indexVreg >= 0)
        return REG_VREG(o->mem.indexVreg);
    return REG_NONE;
}

/* Registro scritto da un'istruzione (dst), NONE se non scrive (o scrive
 * solo FLAGS, non modellato esplicitamente: la macro-fusion CMP/TEST→Jcc
 * è gestita a parte, vedi pinnedForFusion). */
static int instr_def(const MachInstr *in) {
    switch (in->op) {
    case MACH_CMP: case MACH_TEST:
    case MACH_JMP: case MACH_JE: case MACH_JNE:
    case MACH_JL:  case MACH_JLE: case MACH_JG: case MACH_JGE:
    case MACH_CALL: case MACH_RET:
    case MACH_PUSH: case MACH_STORE:
    case MACH_CQO:
    case MACH_LABEL: case MACH_FUNC_BEGIN: case MACH_FUNC_END:
        return REG_NONE;
    default:
        return operand_reg(&in->dst);
    }
}

/* Registri letti da un'istruzione: fino a 4 (src1, src2, eventuale index MEM) */
static void instr_uses(const MachInstr *in, int uses[4], int *nuses) {
    *nuses = 0;
    int r;

    r = operand_reg(&in->src1);
    if (r != REG_NONE) uses[(*nuses)++] = r;
    r = operand_reg2(&in->src1);
    if (r != REG_NONE) uses[(*nuses)++] = r;

    r = operand_reg(&in->src2);
    if (r != REG_NONE) uses[(*nuses)++] = r;
    r = operand_reg2(&in->src2);
    if (r != REG_NONE) uses[(*nuses)++] = r;

    /* STORE/PUSH/IDIV/CQO leggono anche dst (base address, valore, o RAX implicito) */
    switch (in->op) {
    case MACH_STORE:
    case MACH_PUSH:
    case MACH_IDIV:
    case MACH_CQO:
        r = operand_reg(&in->dst);
        if (r != REG_NONE) uses[(*nuses)++] = r;
        break;
    default:
        break;
    }
}

/* =========================================================================
 * Nodo del DAG di dipendenze.
 * ========================================================================= */
#define MAX_SUCCS 32   /* massimo archi uscenti per nodo */

typedef struct {
    int instrIdx;          /* indice nell'array originale del blocco */
    int latency;           /* latenza dell'istruzione */
    int height;             /* cammino critico ponderato verso i sink */
    int succs[MAX_SUCCS];  /* indici dei nodi successori (dipendenti) */
    int nSuccs;
    int predCount;          /* contatore predecessori non ancora emessi */
    int scheduled;          /* 1 se già emesso */

    /*
     * pinnedForFusion: 1 se questo nodo è un CMP/TEST immediatamente
     * seguito da un Jcc nell'ordine originale. Il decodificatore Intel
     * fonde CMP/TEST+Jcc in una singola micro-op SOLO se sono adiacenti:
     * questo nodo va quindi tenuto fuori dalla ready list esattamente
     * come un pinned, cosicché resti non schedulato dal greedy scheduler
     * e venga infine emesso nel pinned-tail insieme al suo Jcc, nello
     * stesso ordine relativo originale (garanzia di adiacenza).
     *
     * Non serve alcun edge dedicato CMP→Jcc né alcuna logica di
     * emissione "a coppia": Jcc è già pinned di suo (vedi is_pinned),
     * quindi finisce anch'esso nel tail; i due, mai schedulati dal
     * greedy, restano vicini nel tail perché il tail scorre gli indici
     * non schedulati in ordine originale.
     */
    int pinnedForFusion;
} DAGNode;

/* =========================================================================
 * Blocco base: sottosequenza contigua di istruzioni tra due LABEL (o
 * inizio/fine funzione).
 * ========================================================================= */
typedef struct {
    int start;   /* indice primo in MachFunction.instrs[] */
    int end;     /* indice dopo l'ultimo (esclusivo) */
} BasicBlock;

/* Restituisce 1 se l'istruzione NON può essere spostata dal greedy
 * scheduler (deve restare nella posizione finale del blocco, nell'ordine
 * originale relativo): label, prologo, terminatori. */
static int is_pinned(MachOp op) {
    switch (op) {
    case MACH_LABEL:
    case MACH_FUNC_BEGIN:
    case MACH_FUNC_END:
    case MACH_JMP:
    case MACH_JE:  case MACH_JNE:
    case MACH_JL:  case MACH_JLE:
    case MACH_JG:  case MACH_JGE:
    case MACH_RET:
        return 1;
    default:
        return 0;
    }
}

/* Effetti collaterali in memoria o flusso di controllo: non riordinabili
 * liberamente tra loro (serializzati con un edge esplicito). */
static int has_side_effect(MachOp op) {
    switch (op) {
    case MACH_STORE: case MACH_PUSH: case MACH_POP:
    case MACH_CALL:
    case MACH_IDIV:   /* scrive RAX e RDX implicitamente */
    case MACH_CQO:    /* scrive RDX implicitamente */
        return 1;
    default:
        return 0;
    }
}

static int is_jcc(MachOp op) {
    switch (op) {
    case MACH_JE: case MACH_JNE:
    case MACH_JL: case MACH_JLE:
    case MACH_JG: case MACH_JGE:
        return 1;
    default:
        return 0;
    }
}

static int is_cmp_or_test(MachOp op) {
    return op == MACH_CMP || op == MACH_TEST;
}

/* =========================================================================
 * Costruisce la lista dei blocchi base per una funzione.
 * ========================================================================= */
static BasicBlock *find_basic_blocks(const MachFunction *f, int *outCount) {
    int cap = 8, count = 0;
    BasicBlock *blocks = malloc((size_t)cap * sizeof(BasicBlock));

    int start = 0;
    for (int i = 0; i < f->count; i++) {
        if (f->instrs[i].op == MACH_LABEL && i > start) {
            if (count == cap) {
                cap *= 2;
                blocks = realloc(blocks, (size_t)cap * sizeof(BasicBlock));
            }
            blocks[count].start = start;
            blocks[count].end   = i;
            count++;
            start = i;
        }
    }
    if (start < f->count) {
        if (count == cap) {
            cap *= 2;
            blocks = realloc(blocks, (size_t)cap * sizeof(BasicBlock));
        }
        blocks[count].start = start;
        blocks[count].end   = f->count;
        count++;
    }

    *outCount = count;
    return blocks;
}

/* =========================================================================
 * Aggiunge un arco dal nodo 'from' al nodo 'to' nel DAG, evitando duplicati.
 * ========================================================================= */
static void dag_add_edge(DAGNode *nodes, int from, int to) {
    if (from == to) return;
    DAGNode *n = &nodes[from];
    for (int i = 0; i < n->nSuccs; i++)
        if (n->succs[i] == to) return;
    if (n->nSuccs < MAX_SUCCS) {
        n->succs[n->nSuccs++] = to;
        nodes[to].predCount++;
    }
}

/* =========================================================================
 * Costruisce il DAG delle dipendenze per le istruzioni di un blocco base.
 *
 * Dipendenze modellate:
 *   RAW: j legge registro scritto da i (i<j) → edge i→j
 *   WAW: j scrive registro già scritto da i (i<j) → edge i→j
 *   WAR: j scrive registro letto da i (i<j) → edge i→j
 *   Side-effect seriale: STORE/CALL/IDIV/CQO → edge tra occorrenze consecutive
 *   Macro-fusion CMP/TEST+Jcc: nessun edge dedicato, vedi pinnedForFusion
 * ========================================================================= */
static void build_dag(const MachFunction *f, int start, int end,
                      DAGNode *nodes) {
    int n = end - start;

    for (int i = 0; i < n; i++) {
        nodes[i].instrIdx        = start + i;
        nodes[i].latency         = latency_of(f->instrs[start + i].op);
        nodes[i].height          = nodes[i].latency;
        nodes[i].nSuccs          = 0;
        nodes[i].predCount       = 0;
        nodes[i].scheduled       = 0;
        nodes[i].pinnedForFusion = 0;
    }

    /* Marca i CMP/TEST immediatamente seguiti da un Jcc: condizione
     * puramente strutturale sull'ordine originale, indipendente da
     * qualunque arco del DAG. */
    for (int i = 0; i < n - 1; i++) {
        if (is_cmp_or_test(f->instrs[start + i].op) &&
            is_jcc(f->instrs[start + i + 1].op)) {
            nodes[i].pinnedForFusion = 1;
        }
    }

    /* lastDef[r] = indice locale (0..n-1) dell'ultima istruzione che ha
     * scritto il registro r in questo blocco; -1 = non ancora definito.
     * Spazio: vreg 0..4095, phys offset 4096..4159. */
    int *lastDef = malloc((size_t)(4096 + 64) * sizeof(int));
    memset(lastDef, -1, (size_t)(4096 + 64) * sizeof(int));

    int lastSideEffect = -1;

    for (int j = 0; j < n; j++) {
        const MachInstr *inj = &f->instrs[start + j];
        int def_j = instr_def(inj);
        int uses_j[4]; int nuses_j;
        instr_uses(inj, uses_j, &nuses_j);

        /* RAW: j legge qualcosa scritto prima */
        for (int u = 0; u < nuses_j; u++) {
            int reg = uses_j[u];
            if (reg == REG_NONE) continue;
            int idx = (reg & 0x3FFFFFFF) + ((reg & 0x40000000) ? 4096 : 0);
            if (idx < 4096 + 64 && lastDef[idx] >= 0)
                dag_add_edge(nodes, lastDef[idx], j);
        }

        /* WAW e WAR: j scrive qualcosa */
        if (def_j != REG_NONE) {
            int idx = (def_j & 0x3FFFFFFF) + ((def_j & 0x40000000) ? 4096 : 0);
            if (idx < 4096 + 64) {
                /* WAW */
                if (lastDef[idx] >= 0)
                    dag_add_edge(nodes, lastDef[idx], j);

                /* WAR: chi ha letto questo registro dall'ultima scrittura
                 * in poi deve completare prima di j. Basta scansionare da
                 * lastDef[idx] (non da 0): i lettori precedenti all'ultima
                 * scrittura sono già ordinati per transitività tramite
                 * l'edge WAW verso quella scrittura stessa. */
                for (int i = (lastDef[idx] >= 0 ? lastDef[idx] : 0); i < j; i++) {
                    int uses_i[4]; int nuses_i;
                    instr_uses(&f->instrs[start + i], uses_i, &nuses_i);
                    for (int u = 0; u < nuses_i; u++) {
                        if (uses_i[u] == def_j)
                            dag_add_edge(nodes, i, j);
                    }
                }

                lastDef[idx] = j;
            }
        }

        /* Side-effect seriale: STORE/CALL/IDIV/CQO non riordinabili tra loro */
        if (has_side_effect(inj->op)) {
            if (lastSideEffect >= 0)
                dag_add_edge(nodes, lastSideEffect, j);
            lastSideEffect = j;
        }
    }

    free(lastDef);

    /* Calcolo altezze (cammino critico ponderato), backward dai sink */
    for (int i = n - 1; i >= 0; i--) {
        int maxSuccHeight = 0;
        for (int s = 0; s < nodes[i].nSuccs; s++) {
            int h = nodes[nodes[i].succs[s]].height;
            if (h > maxSuccHeight) maxSuccHeight = h;
        }
        nodes[i].height = nodes[i].latency + maxSuccHeight;
    }
}

/* =========================================================================
 * List scheduling vero e proprio su un blocco base.
 *
 * Algoritmo:
 *   1. Pinned head (LABEL/FUNC_BEGIN): emessi subito, in ordine originale.
 *   2. Ready list iniziale: nodi non pinnati, non pinnedForFusion, senza
 *      predecessori.
 *   3. Greedy: emetti il nodo ready con altezza (cammino critico) massima,
 *      aggiorna predCount dei successori.
 *   4. Pinned tail: tutto ciò che non è stato schedulato dal greedy
 *      (terminatori pinnati + CMP/TEST pinnedForFusion) viene emesso in
 *      ordine originale — questo è ciò che garantisce l'adiacenza
 *      CMP/TEST+Jcc per la macro-fusion, senza bisogno di alcun
 *      meccanismo dedicato di "emissione a coppia".
 * ========================================================================= */
static void schedule_block(MachFunction *f, int start, int end) {
    int n = end - start;
    if (n <= 1) return;   /* blocco triviale, nulla da fare */

    DAGNode *nodes = malloc((size_t)n * sizeof(DAGNode));
    build_dag(f, start, end, nodes);

    MachInstr *result = malloc((size_t)n * sizeof(MachInstr));
    int rCount = 0;

    /* Ready list: array semplice con ricerca del massimo O(|ready|) — i
     * blocchi base sono piccoli (tipicamente < 50 istruzioni), un heap
     * non porta vantaggi misurabili e complicherebbe il codice. */
    int *ready = malloc((size_t)n * sizeof(int));
    int  readyCount = 0;

    /* Pinned head */
    for (int i = 0; i < n; i++) {
        MachOp op = f->instrs[start + i].op;
        if (op == MACH_LABEL || op == MACH_FUNC_BEGIN) {
            result[rCount++] = f->instrs[start + i];
            nodes[i].scheduled = 1;
        }
    }

    /* Ready list iniziale */
    for (int i = 0; i < n; i++) {
        if (nodes[i].scheduled) continue;
        if (is_pinned(f->instrs[start + i].op)) continue;
        if (nodes[i].pinnedForFusion) continue;
        if (nodes[i].predCount == 0)
            ready[readyCount++] = i;
    }

    /* Scheduling greedy: priorità = altezza (cammino critico rimanente) */
    while (readyCount > 0) {
        int bestIdx = 0;
        for (int i = 1; i < readyCount; i++) {
            if (nodes[ready[i]].height > nodes[ready[bestIdx]].height)
                bestIdx = i;
        }
        int chosen = ready[bestIdx];
        ready[bestIdx] = ready[--readyCount];

        result[rCount++] = f->instrs[start + chosen];
        nodes[chosen].scheduled = 1;

        for (int s = 0; s < nodes[chosen].nSuccs; s++) {
            int succ = nodes[chosen].succs[s];
            if (nodes[succ].scheduled) continue;
            if (is_pinned(f->instrs[start + succ].op)) continue;
            if (nodes[succ].pinnedForFusion) continue;
            nodes[succ].predCount--;
            if (nodes[succ].predCount == 0)
                ready[readyCount++] = succ;
        }
    }

    /* Pinned tail: tutto ciò che resta non schedulato (terminatori pinnati
     * + eventuali CMP/TEST pinnedForFusion), in ordine originale. Questo
     * preserva l'adiacenza CMP/TEST+Jcc: nessuno dei due è mai entrato nel
     * greedy, quindi restano vicini esattamente come nell'originale. */
    for (int i = 0; i < n; i++) {
        if (nodes[i].scheduled) continue;
        result[rCount++] = f->instrs[start + i];
        nodes[i].scheduled = 1;
    }

    memcpy(&f->instrs[start], result, (size_t)n * sizeof(MachInstr));

    free(result);
    free(ready);
    free(nodes);
}

/* =========================================================================
 * Entry point pubblico: schedula tutte le funzioni del programma.
 * ========================================================================= */
void sched_schedule(MachProgram *mp) {
    for (int fi = 0; fi < mp->count; fi++) {
        MachFunction *f = mp->functions[fi];
        if (!f || f->count == 0) continue;

        int bbCount = 0;
        BasicBlock *blocks = find_basic_blocks(f, &bbCount);

        for (int b = 0; b < bbCount; b++)
            schedule_block(f, blocks[b].start, blocks[b].end);

        free(blocks);
    }
}