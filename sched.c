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
 * Usiamo un intero a 32 bit: bit[31]=0 vreg, bit[31]=1 phys.
 * -1 = nessun registro.
 * ========================================================================= */
#define REG_NONE   (-1)
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

/* Registro scritto da un'istruzione (dst), NONE se non scrive */
static int instr_def(const MachInstr *in) {
    switch (in->op) {
    /* Istruzioni che non scrivono dst (o dst è FLAGS implicito) */
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

/* Registri letti da un'istruzione: fino a 3 (src1, src2, eventuale base MEM) */
static void instr_uses(const MachInstr *in, int uses[4], int *nuses) {
    *nuses = 0;
    int r;

    /* src1 */
    r = operand_reg(&in->src1);
    if (r != REG_NONE) uses[(*nuses)++] = r;
    r = operand_reg2(&in->src1);
    if (r != REG_NONE) uses[(*nuses)++] = r;

    /* src2 */
    r = operand_reg(&in->src2);
    if (r != REG_NONE) uses[(*nuses)++] = r;
    r = operand_reg2(&in->src2);
    if (r != REG_NONE) uses[(*nuses)++] = r;

    /* Per STORE e PUSH anche dst è letto (contiene il base address o il valore) */
    switch (in->op) {
    case MACH_STORE:
    case MACH_PUSH:
    case MACH_IDIV:   /* divide implicitamente RAX:RDX */
    case MACH_CQO:    /* legge RAX */
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
    int          instrIdx;          /* indice nell'array originale del blocco */
    int          latency;           /* latenza dell'istruzione */
    int          height;            /* cammino critico ponderato verso i sink */
    int          succs[MAX_SUCCS];  /* indici dei nodi successori (dipendenti) */
    int          nSuccs;
    int          predCount;         /* contatore predecessori non ancora emessi */
    int          scheduled;         /* 1 se già emesso */
    /*
     * Macro-fusion ghost node.
     *
     * fusion_partner >= 0: questo nodo è un CMP/TEST che deve essere emesso
     * atomicamente PRIMA del Jcc indicato da fusion_partner.
     * Il nodo viene rimosso dalla ready list normale e non viene mai
     * selezionato dallo scheduler greedy. Viene emesso solo in coppia
     * quando il Jcc corrispondente viene scelto (vedi emit_chosen).
     *
     * -1 = nodo normale, nessun legame di fusione.
     */
    int          fusion_partner;    /* indice locale del Jcc da trascinare, o -1 */
} DAGNode;

/* =========================================================================
 * Blocco base: sottosequenza contigua di istruzioni tra due LABEL (o
 * inizio/fine funzione). LABEL di apertura e terminatori (JMP, Jcc, RET,
 * CALL) sono inclusi nel blocco ma trattati come ancorati.
 * ========================================================================= */
typedef struct {
    int start;   /* indice primo in MachFunction.instrs[] */
    int end;     /* indice dopo l'ultimo (esclusivo) */
} BasicBlock;

/* Restituisce 1 se l'istruzione NON può essere spostata (deve restare
 * in posizione fissa all'interno del blocco). */
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

/* Restituisce 1 se l'istruzione ha effetti collaterali in memoria o
 * flusso di controllo (non può essere riordinata liberamente con altri). */
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

/* =========================================================================
 * Identifica se l'istruzione j è un salto condizionato (Jcc).
 * ========================================================================= */
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

/* =========================================================================
 * Costruisce la lista dei blocchi base per una funzione.
 * Un nuovo blocco inizia:
 *   - all'istruzione 0
 *   - dopo ogni MACH_LABEL (il label apre un nuovo blocco)
 * Un blocco termina:
 *   - all'ultima istruzione della funzione
 *   - all'istruzione prima di un MACH_LABEL
 * ========================================================================= */
static BasicBlock *find_basic_blocks(const MachFunction *f, int *outCount) {
    int cap = 8, count = 0;
    BasicBlock *blocks = malloc((size_t)cap * sizeof(BasicBlock));

    int start = 0;
    for (int i = 0; i < f->count; i++) {
        /* Un LABEL (che non sia il primo) chiude il blocco precedente
         * e apre uno nuovo. */
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
    /* Ultimo blocco */
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
 * Aggiunge un arco dal nodo 'from' al nodo 'to' nel DAG, evitando
 * duplicati.
 * ========================================================================= */
static void dag_add_edge(DAGNode *nodes, int from, int to) {
    if (from == to) return;
    DAGNode *n = &nodes[from];
    for (int i = 0; i < n->nSuccs; i++)
        if (n->succs[i] == to) return;   /* già presente */
    if (n->nSuccs < MAX_SUCCS) {
        n->succs[n->nSuccs++] = to;
        nodes[to].predCount++;
    }
}

/* =========================================================================
 * Costruisce il DAG delle dipendenze per le istruzioni di un blocco base.
 *
 * Dipendenze modellate:
 *   RAW: istruzione j legge registro scritto da i (i < j) → edge i→j
 *   WAW: istruzione j scrive registro scritto da i (i < j) → edge i→j
 *   WAR: istruzione j scrive registro letto da i (i < j) → edge i→j
 *   Side-effect seriale: due istruzioni con side effect → edge i→j
 *   Macro-fusion: CMP/TEST immediatamente prima di Jcc → edge hard
 *
 * 'nodes' è un array già allocato di dimensione (end-start).
 * ========================================================================= */
static void build_dag(const MachFunction *f, int start, int end,
                      DAGNode *nodes) {
    int n = end - start;

    /* Inizializza nodi */
    for (int i = 0; i < n; i++) {
        nodes[i].instrIdx       = start + i;
        nodes[i].latency        = latency_of(f->instrs[start + i].op);
        nodes[i].height         = nodes[i].latency;
        nodes[i].nSuccs         = 0;
        nodes[i].predCount      = 0;
        nodes[i].scheduled      = 0;
        nodes[i].fusion_partner = -1;
    }

    /* Ultimo indice di scrittura per ogni registro visto.
     * Usa una tabella piatta: max vreg stimato 4096, phys 16.
     * lastDef[r] = indice del nodo (locale, 0..n-1) che ha definito r per ultimo.
     * -1 = non ancora definito nel blocco. */
    int *lastDef = malloc((size_t)(4096 + 64) * sizeof(int));
    memset(lastDef, -1, (size_t)(4096 + 64) * sizeof(int));

    /* lastSideEffect: indice dell'ultimo nodo con side effect visto */
    int lastSideEffect = -1;

    /* Per ogni coppia di istruzioni, controlla dipendenze RAW/WAW/WAR */
    for (int j = 0; j < n; j++) {
        const MachInstr *inj = &f->instrs[start + j];
        int def_j = instr_def(inj);
        int uses_j[4]; int nuses_j;
        instr_uses(inj, uses_j, &nuses_j);

        /* RAW: j legge qualcosa che è stato scritto prima */
        for (int u = 0; u < nuses_j; u++) {
            int reg = uses_j[u];
            if (reg == REG_NONE) continue;
            int idx = (reg & 0x3FFFFFFF);
            if (reg & 0x40000000) idx += 4096;   /* phys offset */
            if (idx < 4096 + 64 && lastDef[idx] >= 0)
                dag_add_edge(nodes, lastDef[idx], j);
        }

        /* WAW e WAR: j scrive qualcosa */
        if (def_j != REG_NONE) {
            int idx = (def_j & 0x3FFFFFFF);
            if (def_j & 0x40000000) idx += 4096;

            /* WAW: j sovrascrive stessa destinazione di una def precedente */
            if (idx < 4096 + 64 && lastDef[idx] >= 0)
                dag_add_edge(nodes, lastDef[idx], j);

            /* WAR: qualcuno prima di j leggeva questo registro;
             * scansiona all'indietro finché non trovi il lastDef o l'inizio */
            for (int i = (lastDef[idx] >= 0 ? lastDef[idx] : 0); i < j; i++) {
                int uses_i[4]; int nuses_i;
                instr_uses(&f->instrs[start + i], uses_i, &nuses_i);
                for (int u = 0; u < nuses_i; u++) {
                    if (uses_i[u] == def_j)
                        dag_add_edge(nodes, i, j);
                }
            }

            /* Aggiorna lastDef */
            if (idx < 4096 + 64) lastDef[idx] = j;
        }

        /* Side-effect seriale: STORE/CALL/IDIV/CQO non riordinabili tra loro */
        if (has_side_effect(inj->op)) {
            if (lastSideEffect >= 0)
                dag_add_edge(nodes, lastSideEffect, j);
            lastSideEffect = j;
        }

        /*
         * Macro-fusion: CMP/TEST immediatamente prima di un Jcc.
         *
         * PROBLEMA con il solo edge DAG: l'edge CMP→JCC garantisce
         * solo che JCC venga emesso dopo CMP, non che siano adiacenti.
         * Lo scheduler può inserire altre istruzioni ready-in-mezzo.
         *
         * SOLUZIONE — nodo fantasma (ghost node):
         *   Se j è un Jcc E j-1 è CMP/TEST, il nodo j-1 diventa un
         *   ghost: viene escluso dalla ready list normale e non è mai
         *   scelto dallo scheduler greedy. Viene emesso in coppia
         *   (atomicamente) subito prima che j venga emesso.
         *
         *   Inoltre tutti gli archi entranti in j-1 (dipendenze che
         *   altri hanno sul CMP) vengono trasferiti su j, così j
         *   eredita i vincoli di ordinamento del ghost.
         */
        if (j > 0 && is_jcc(inj->op)) {
            MachOp prev = f->instrs[start + j - 1].op;
            if (prev == MACH_CMP || prev == MACH_TEST) {
                int cmp_node = j - 1;
                int jcc_node = j;

                /* Marca il CMP come ghost: il suo Jcc lo trarrà con sé */
                nodes[cmp_node].fusion_partner = jcc_node;

                /* Assicura l'edge ordinamento CMP → JCC nel DAG */
                dag_add_edge(nodes, cmp_node, jcc_node);

                /*
                 * Trasferisci sul Jcc tutti gli archi ENTRANTI nel CMP
                 * che vengono da nodi diversi dal Jcc stesso.
                 * In questo modo, chiunque debba completare prima del CMP
                 * deve completare anche prima del Jcc — e quindi prima
                 * che la coppia venga emessa insieme.
                 *
                 * Nota: non possiamo iterare sul DAG invertito perché
                 * non lo costruiamo esplicitamente. Riscansiona i nodi
                 * precedenti che hanno un arco verso cmp_node.
                 */
                for (int k = 0; k < cmp_node; k++) {
                    for (int s = 0; s < nodes[k].nSuccs; s++) {
                        if (nodes[k].succs[s] == cmp_node) {
                            dag_add_edge(nodes, k, jcc_node);
                            break;
                        }
                    }
                }
            }
        }
    }

    free(lastDef);

    /* ---- Calcolo altezze (cammino critico ponderato) ----
     * height[i] = latency[i] + max(height[succ] per ogni succ di i)
     * Calcolato in ordine inverso (dai sink ai source). */
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
 *   1. Separa le istruzioni pinnate (LABEL, terminatori) dalle schedulabili.
 *   2. Inizializza ready list con i nodi senza predecessori.
 *   3. Itera: prendi il nodo ready con altezza massima (critical path first),
 *      emettilo, decrementa predCount dei successori, aggiunge i nuovi ready.
 *   4. Reinserisce LABEL all'inizio e terminatori alla fine.
 *
 * Garanzia macro-fusion: i nodi CMP/TEST che hanno un unico successore Jcc
 * vengono emessi immediatamente prima di quel Jcc (il DAG lo garantisce
 * perché il Jcc non entra in ready finché CMP non è emesso).
 * ========================================================================= */
static void schedule_block(MachFunction *f, int start, int end) {
    int n = end - start;
    if (n <= 1) return;   /* blocco triviale, nulla da fare */

    DAGNode *nodes = malloc((size_t)n * sizeof(DAGNode));
    build_dag(f, start, end, nodes);

    /* Array di output per le istruzioni riordinate */
    MachInstr *result = malloc((size_t)n * sizeof(MachInstr));
    int rCount = 0;

    /* Ready list: array di indici locali di nodi pronti.
     * Usiamo un array semplice con ricerca del massimo O(|ready|):
     * i blocchi base sono piccoli (tipicamente < 50 istruzioni), quindi
     * un heap non porta vantaggi misurabili e complicherebbe il codice. */
    int *ready = malloc((size_t)n * sizeof(int));
    int  readyCount = 0;

    /* Pinned head (LABEL, FUNC_BEGIN): emessi subito, non schedulati */
    for (int i = 0; i < n; i++) {
        const MachInstr *in = &f->instrs[start + i];
        if (in->op == MACH_LABEL || in->op == MACH_FUNC_BEGIN) {
            result[rCount++] = *in;
            nodes[i].scheduled = 1;
        }
    }

    /*
     * Inizializza ready list con i nodi non pinnati e senza predecessori.
     *
     * I ghost node (fusion_partner >= 0, ovvero CMP/TEST legati a un Jcc)
     * NON entrano nella ready list normale: verranno emessi atomicamente
     * subito prima del loro Jcc partner.
     */
    for (int i = 0; i < n; i++) {
        if (nodes[i].scheduled) continue;
        if (is_pinned(f->instrs[start + i].op)) continue;
        if (nodes[i].fusion_partner >= 0) continue;   /* ghost: escludi */
        if (nodes[i].predCount == 0)
            ready[readyCount++] = i;
    }

    /* Scheduling greedy: priorità = altezza (cammino critico rimanente) */
    while (readyCount > 0) {
        /* Trova il nodo con altezza massima nella ready list */
        int bestIdx = 0;
        for (int i = 1; i < readyCount; i++) {
            if (nodes[ready[i]].height > nodes[ready[bestIdx]].height)
                bestIdx = i;
        }
        int chosen = ready[bestIdx];

        /* Rimuovi dalla ready list (swap con ultimo) */
        ready[bestIdx] = ready[--readyCount];

        /*
         * Emissione atomica macro-fusion:
         * Se il nodo scelto è un Jcc con un ghost partner (CMP/TEST),
         * emetti prima il ghost e poi il Jcc — garantendo adiacenza.
         *
         * Il ghost potrebbe ancora avere predCount > 0 in teoria (dipendenze
         * da nodi non ancora emessi). Se questo accade c'è un problema nel
         * DAG, ma lo gestiamo in modo sicuro: emettiamo comunque il ghost
         * perché la coppia DEVE essere atomica per la correttezza della
         * macro-fusion; le dipendenze del ghost sono state già trasferite
         * sul Jcc durante build_dag, quindi l'ordine globale è corretto.
         */
        for (int i = 0; i < n; i++) {
            /* Cerca un ghost che punta a 'chosen' come partner */
            if (nodes[i].fusion_partner == chosen && !nodes[i].scheduled) {
                result[rCount++] = f->instrs[start + i];
                nodes[i].scheduled = 1;
                /* Aggiorna successori del ghost (che include chosen stesso) */
                for (int s = 0; s < nodes[i].nSuccs; s++) {
                    int succ = nodes[i].succs[s];
                    if (nodes[succ].scheduled) continue;
                    if (is_pinned(f->instrs[start + succ].op)) continue;
                    if (nodes[succ].fusion_partner >= 0) continue;
                    nodes[succ].predCount--;
                    if (nodes[succ].predCount == 0)
                        ready[readyCount++] = succ;
                }
                break;   /* al massimo un ghost per Jcc */
            }
        }

        /* Emetti il nodo scelto (Jcc o qualsiasi altro) */
        result[rCount++] = f->instrs[start + chosen];
        nodes[chosen].scheduled = 1;

        /* Aggiorna predecessori dei successori */
        for (int s = 0; s < nodes[chosen].nSuccs; s++) {
            int succ = nodes[chosen].succs[s];
            if (nodes[succ].scheduled) continue;
            if (is_pinned(f->instrs[start + succ].op)) continue;
            if (nodes[succ].fusion_partner >= 0) continue;  /* ghost: mai in ready */
            nodes[succ].predCount--;
            if (nodes[succ].predCount == 0)
                ready[readyCount++] = succ;
        }
    }

    /* Pinned tail: terminatori (JMP, Jcc, RET) emessi alla fine, in ordine */
    for (int i = 0; i < n; i++) {
        if (nodes[i].scheduled) continue;
        /* Devono essere terminatori pinnati non ancora emessi */
        result[rCount++] = f->instrs[start + i];
        nodes[i].scheduled = 1;
    }

    /* Copia il risultato nel buffer originale */
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