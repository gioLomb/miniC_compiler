#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "instr_selector.h"

/* =========================================================================
 * Costruttori operandi — helper inline per leggibilità
 * ========================================================================= */

static inline MachOperand mo_none(void) {
    MachOperand o; o.kind = MO_NONE; return o;
}
static inline MachOperand mo_vreg(int id) {
    MachOperand o; o.kind = MO_VREG; o.vregId = id; return o;
}
static inline MachOperand mo_phys(MachPhysReg r) {
    MachOperand o; o.kind = MO_PHYS; o.physReg = (int)r; return o;
}
static inline MachOperand mo_imm(long v) {
    MachOperand o; o.kind = MO_IMM; o.imm = v; return o;
}
static inline MachOperand mo_label(int id) {
    MachOperand o; o.kind = MO_LABEL; o.labelId = id; return o;
}
static inline MachOperand mo_func(const char *name) {
    MachOperand o; o.kind = MO_FUNC; o.func = name; return o;
}
static inline MachOperand mo_mem(int base, int index, int scale, int disp) {
    MachOperand o;
    o.kind = MO_MEM;
    o.mem.baseVreg  = base;
    o.mem.indexVreg = index;
    o.mem.scale     = scale;
    o.mem.disp      = disp;
    return o;
}

/* =========================================================================
 * MachFunction: gestione dinamica array istruzioni
 * ========================================================================= */

static MachFunction *mfunc_create(const char *name) {
    MachFunction *f = calloc(1, sizeof(MachFunction));
    f->name     = name;   /* stringa non-owning: punta a IRFunction->name */
    f->capacity = 64;
    f->instrs   = malloc((size_t)f->capacity * sizeof(MachInstr));
    f->nextVreg = 0;
    return f;
}

static void mfunc_emit(MachFunction *f, MachOp op,
                        MachOperand dst, MachOperand src1, MachOperand src2) {
    if (f->count == f->capacity) {
        f->capacity *= 2;
        f->instrs = realloc(f->instrs, (size_t)f->capacity * sizeof(MachInstr));
    }
    MachInstr *in = &f->instrs[f->count++];
    in->op   = op;
    in->dst  = dst;
    in->src1 = src1;
    in->src2 = src2;
    in->scale = 8;   /* default: elementi da 8 byte (int/ptr) */
}

static inline int mfunc_new_vreg(MachFunction *f) {
    return f->nextVreg++;
}

/* =========================================================================
 * VarMap: Operand IR → virtual register ID
 *
 * Ogni OPND_VAR e OPND_TEMP riceve un vreg univoco al primo incontro.
 * Struttura semplice: array di record (chiave, vregId) con linear scan.
 * Sufficiente per funzioni piccole; per funzioni grandi sarebbe una hash.
 * ========================================================================= */

#define VARMAP_MAX 1024

typedef struct {
    int kind;       /* 0=var, 1=temp */
    int a;          /* varLevel o tempId */
    int b;          /* varOffset (solo per var) */
    int vregId;
} VMapEntry;

typedef struct {
    VMapEntry entries[VARMAP_MAX];
    int       count;
} VarMap;

static int vmap_lookup(VarMap *vm, int kind, int a, int b) {
    for (int i = 0; i < vm->count; i++) {
        VMapEntry *e = &vm->entries[i];
        if (e->kind == kind && e->a == a && e->b == b)
            return e->vregId;
    }
    return -1;
}

static int vmap_get_or_create(VarMap *vm, MachFunction *f, int kind, int a, int b) {
    int id = vmap_lookup(vm, kind, a, b);
    if (id >= 0) return id;
    id = mfunc_new_vreg(f);
    if (vm->count < VARMAP_MAX) {
        VMapEntry *e = &vm->entries[vm->count++];
        e->kind = kind; e->a = a; e->b = b; e->vregId = id;
    }
    return id;
}

/* Traduce un Operand IR in vreg ID (-1 se non è var/temp) */
static int operand_to_vreg(const Operand *op, VarMap *vm, MachFunction *f) {
    if (op->kind == OPND_VAR)
        return vmap_get_or_create(vm, f, 0, op->data.varLevel, op->data.varOffset);
    if (op->kind == OPND_TEMP)
        return vmap_get_or_create(vm, f, 1, op->data.tempId, 0);
    return -1;
}

/* =========================================================================
 * Caricamento operando IR → vreg (emette MOV se necessario)
 *
 * Restituisce il vreg che contiene il valore dell'operando.
 * Per costanti: crea vreg fresco e vi carica l'immediato.
 * Per var/temp: restituisce direttamente il vreg associato.
 * ========================================================================= */

static int load_operand(const Operand *op, VarMap *vm, MachFunction *f) {
    switch (op->kind) {
    case OPND_VAR:
    case OPND_TEMP:
        return operand_to_vreg(op, vm, f);

    case OPND_CONST_INT: {
        int dst = mfunc_new_vreg(f);
        mfunc_emit(f, MACH_MOV, mo_vreg(dst), mo_imm(op->data.intVal), mo_none());
        return dst;
    }
    case OPND_CONST_FLOAT: {
        /* Float: per ora trattato come intero bit-pattern — placeholder.
           Corretto quando si aggiunge la gestione SSE. */
        int dst = mfunc_new_vreg(f);
        union { float f; int i; } u; u.f = op->data.floatVal;
        mfunc_emit(f, MACH_MOV, mo_vreg(dst), mo_imm(u.i), mo_none());
        return dst;
    }
    default:
        return -1;   /* OPND_NONE, OPND_LABEL, OPND_FUNC: non caricabili così */
    }
}

/* =========================================================================
 * operand_to_mach: converte Operand IR in MachOperand SENZA materializzare
 * costanti in vreg. Usare dove x86-64 accetta immediati (MOV src, ADD rhs,
 * CMP rhs, PUSH...). Per operazioni che richiedono registro (IDIV divisore,
 * TEST, unari in-place) continuare a usare load_operand.
 * ========================================================================= */

static MachOperand operand_to_mach(const Operand *op, VarMap *vm,
                                   MachFunction *mf) {
    switch (op->kind) {
    case OPND_CONST_INT:
        return mo_imm(op->data.intVal);
    case OPND_CONST_FLOAT: {
        union { float f; int i; } u;
        u.f = op->data.floatVal;
        return mo_imm(u.i);
    }
    case OPND_VAR:
    case OPND_TEMP:
        return mo_vreg(operand_to_vreg(op, vm, mf));
    default:
        return mo_none();
    }
}

/* Inverte l'operatore di confronto (per CMP con operandi scambiati) */
static IROp flip_cmp(IROp op) {
    switch (op) {
    case IR_LT: return IR_GT;
    case IR_GT: return IR_LT;
    case IR_LE: return IR_GE;
    case IR_GE: return IR_LE;
    default:    return op;   /* IR_EQ, IR_NE: simmetrici */
    }
}

/* =========================================================================
 * Peephole helper: rilevamento pattern per fusione
 * ========================================================================= */

/* Ritorna il MachOp setcc corrispondente all'IROp di confronto */
static MachOp comparison_to_setcc(IROp cmpOp) {
    switch (cmpOp) {
    case IR_LT: return MACH_SETL;
    case IR_LE: return MACH_SETLE;
    case IR_GT: return MACH_SETG;
    case IR_GE: return MACH_SETGE;
    case IR_EQ: return MACH_SETE;
    case IR_NE: return MACH_SETNE;
    default:    return MACH_SETE;
    }
}

/* Controlla se K è potenza di 2 e ne ritorna il log2 (-1 se no) */
static int log2_if_pow2(long k) {
    if (k <= 0 || (k & (k - 1)) != 0) return -1;
    int n = 0;
    while ((k >> n) > 1) n++;
    return n;
}

/* =========================================================================
 * Convenzione chiamata System V AMD64:
 * Argomenti interi: rdi, rsi, rdx, rcx, r8, r9 (poi stack)
 * ========================================================================= */
static const MachPhysReg ARG_REGS[] = {
    PHYS_RDI, PHYS_RSI, PHYS_RDX, PHYS_RCX, PHYS_R8, PHYS_R9
};
#define NUM_ARG_REGS 6

/* =========================================================================
 * Selezione istruzioni: core
 *
 * Itera le IR instructions della funzione.
 * Stato condiviso tra iterazioni:
 *   - pending_params: argomenti accumulati per la prossima IR_CALL
 *   - prev_cmp_*: info sull'istruzione precedente per CMP+Jcc fusion
 * ========================================================================= */

/* Stato del "pending comparison" per CMP+Jcc fusion.
 * Quando si espande IR_LT/IR_EQ/ecc., invece di emettere subito SETCC,
 * si salva il contesto e si aspetta: se l'istruzione successiva è
 * IR_IF_FALSE sul dst della comparazione, si fonde in CMP+JCC. */
typedef struct {
    int   active;       /* 1 se c'è una comparazione pendente */
    IROp  cmpOp;        /* IR_LT, IR_EQ, ecc. */
    int   src1Vreg;     /* vreg del primo operando del cmp */
    int   src2Vreg;     /* vreg del secondo operando */
    int   dstVreg;      /* vreg dove andrebbe il booleano materializzato */
} PendingCmp;

#define MAX_PARAMS 64

static MachFunction *select_function(const IRFunction *irf) {
    MachFunction *f  = mfunc_create(irf->name);
    VarMap vm; vm.count = 0;

    /* Prologo: marcatore, la dimensione frame viene riempita dopo */
    mfunc_emit(f, MACH_FUNC_BEGIN, mo_none(), mo_none(), mo_none());

    /* Buffer argomenti PARAM accumulati prima di ogni CALL */
    int   param_vregs[MAX_PARAMS];
    int   param_count = 0;

    PendingCmp pcmp; pcmp.active = 0;

    for (int i = 0; i < irf->count; i++) {
        const IRInstr *in = &irf->instrs[i];

        /* ----------------------------------------------------------------
         * Se l'istruzione corrente NON è IF_FALSE, oppure l'IF_FALSE non
         * legge il dst della comparazione pendente → materializza il
         * booleano della comparazione pendente (non c'è fusione).
         * ---------------------------------------------------------------- */
        if (pcmp.active) {
            /* Verifica se l'istruzione corrente è un IF_FALSE che consuma
             * esattamente il dst della comparazione pendente → fusione CMP+JCC.
             * Altrimenti materializza il booleano prima di procedere. */
            int must_materialize = 1;
            if (in->op == IR_IF_FALSE) {
                int cond_vreg = operand_to_vreg(&in->src1, &vm, f);
                must_materialize = (cond_vreg != pcmp.dstVreg);
            }

            if (must_materialize) {
                /* Emetti CMP + SETCC + MOVSX per produrre il booleano */
                mfunc_emit(f, MACH_CMP,
                           mo_vreg(pcmp.src1Vreg), mo_vreg(pcmp.src2Vreg), mo_none());
                MachOp setcc = comparison_to_setcc(pcmp.cmpOp);
                mfunc_emit(f, setcc, mo_phys(PHYS_AL), mo_none(), mo_none());
                mfunc_emit(f, MACH_MOVSX,
                           mo_vreg(pcmp.dstVreg), mo_phys(PHYS_AL), mo_none());
                pcmp.active = 0;
            }
        }

        switch (in->op) {

        /* ----------------------------------------------------------------
         * Etichette e salti
         * ---------------------------------------------------------------- */
        case IR_LABEL:
            mfunc_emit(f, MACH_LABEL, mo_label(in->dst.data.labelId), mo_none(), mo_none());
            break;

        case IR_GOTO:
            mfunc_emit(f, MACH_JMP, mo_label(in->dst.data.labelId), mo_none(), mo_none());
            break;

        case IR_IF_FALSE: {
            int cond_vreg = operand_to_vreg(&in->src1, &vm, f);
            int lbl       = in->dst.data.labelId;

            if (pcmp.active && cond_vreg == pcmp.dstVreg) {
                /* === CMP+JCC FUSION === */
                /* Invertiamo la condizione: if_false salta se falso,
                 * quindi usiamo il Jcc "inverso" (es. LT → JGE per saltare) */
                MachOp jcc_fused;
                switch (pcmp.cmpOp) {
                case IR_LT: jcc_fused = MACH_JGE; break;
                case IR_LE: jcc_fused = MACH_JG;  break;
                case IR_GT: jcc_fused = MACH_JLE; break;
                case IR_GE: jcc_fused = MACH_JL;  break;
                case IR_EQ: jcc_fused = MACH_JNE; break;
                case IR_NE: jcc_fused = MACH_JE;  break;
                default:    jcc_fused = MACH_JMP;  break;
                }
                mfunc_emit(f, MACH_CMP,
                           mo_vreg(pcmp.src1Vreg), mo_vreg(pcmp.src2Vreg), mo_none());
                mfunc_emit(f, jcc_fused, mo_label(lbl), mo_none(), mo_none());
                pcmp.active = 0;
            } else {
                /* Caso generale: TEST cond,cond + JE */
                mfunc_emit(f, MACH_TEST, mo_vreg(cond_vreg), mo_vreg(cond_vreg), mo_none());
                mfunc_emit(f, MACH_JE, mo_label(lbl), mo_none(), mo_none());
            }
            break;
        }

        /* ----------------------------------------------------------------
         * Assegnamento semplice: src può essere immediato direttamente
         * ---------------------------------------------------------------- */
        case IR_ASSIGN: {
            int         dst = operand_to_vreg(&in->dst, &vm, f);
            MachOperand src = operand_to_mach(&in->src1, &vm, f);
            mfunc_emit(f, MACH_MOV, mo_vreg(dst), src, mo_none());
            break;
        }

        /* ----------------------------------------------------------------
         * Aritmetica binaria: ADD, SUB
         * MOV accetta immediato come src; ADD/SUB accettano immediato come rhs.
         * ---------------------------------------------------------------- */
        case IR_ADD:
        case IR_SUB: {
            int         dst = operand_to_vreg(&in->dst, &vm, f);
            MachOperand lhs = operand_to_mach(&in->src1, &vm, f);
            MachOperand rhs = operand_to_mach(&in->src2, &vm, f);
            int         tmp = mfunc_new_vreg(f);
            mfunc_emit(f, MACH_MOV,
                       mo_vreg(tmp), lhs, mo_none());
            mfunc_emit(f, in->op == IR_ADD ? MACH_ADD : MACH_SUB,
                       mo_vreg(tmp), rhs, mo_none());
            mfunc_emit(f, MACH_MOV,
                       mo_vreg(dst), mo_vreg(tmp), mo_none());
            break;
        }

        /* ----------------------------------------------------------------
         * Moltiplicazione: MUL
         * Peephole: se src2 è costante potenza di 2 → SAL
         * ---------------------------------------------------------------- */
        case IR_MUL: {
            int dst = operand_to_vreg(&in->dst, &vm, f);

            /* Controlla se uno degli operandi è costante potenza di 2 */
            long const_val = 0;
            int  is_pow2   = 0;
            int  lhs_idx   = -1;  /* indice dell'operando non-costante */

            if (in->src2.kind == OPND_CONST_INT) {
                const_val = in->src2.data.intVal;
                is_pow2   = (log2_if_pow2(const_val) >= 0);
                lhs_idx   = 0;   /* non-costante è src1 */
            } else if (in->src1.kind == OPND_CONST_INT) {
                const_val = in->src1.data.intVal;
                is_pow2   = (log2_if_pow2(const_val) >= 0);
                lhs_idx   = 1;   /* non-costante è src2 */
            }

            if (is_pow2) {
                int shift = log2_if_pow2(const_val);
                const Operand *non_const = (lhs_idx == 0) ? &in->src1 : &in->src2;
                int lhs = load_operand(non_const, &vm, f);
                int tmp = mfunc_new_vreg(f);
                mfunc_emit(f, MACH_MOV, mo_vreg(tmp), mo_vreg(lhs), mo_none());
                mfunc_emit(f, MACH_SAL, mo_vreg(tmp), mo_imm(shift), mo_none());
                mfunc_emit(f, MACH_MOV, mo_vreg(dst), mo_vreg(tmp), mo_none());
            } else {
                int lhs = load_operand(&in->src1, &vm, f);
                int rhs = load_operand(&in->src2, &vm, f);
                /* IMUL a 2 operandi: tmp = lhs; tmp *= rhs */
                int tmp = mfunc_new_vreg(f);
                mfunc_emit(f, MACH_MOV, mo_vreg(tmp), mo_vreg(lhs), mo_none());
                mfunc_emit(f, MACH_IMUL, mo_vreg(tmp), mo_vreg(rhs), mo_none());
                mfunc_emit(f, MACH_MOV, mo_vreg(dst), mo_vreg(tmp), mo_none());
            }
            break;
        }

        /* ----------------------------------------------------------------
         * Divisione e modulo: richiedono rax/rdx
         * Pattern: mov lhs→rax; cqo; idiv rhs_reg; mov rax/rdx→dst
         * ---------------------------------------------------------------- */
        case IR_DIV:
        case IR_MOD: {
            int dst = operand_to_vreg(&in->dst, &vm, f);
            int lhs = load_operand(&in->src1, &vm, f);
            int rhs = load_operand(&in->src2, &vm, f);

            /* lhs → rax */
            mfunc_emit(f, MACH_MOV, mo_phys(PHYS_RAX), mo_vreg(lhs), mo_none());
            /* sign-extend rax → rdx:rax */
            mfunc_emit(f, MACH_CQO, mo_none(), mo_none(), mo_none());
            /* idiv rhs (divisore non può essere immediato) */
            mfunc_emit(f, MACH_IDIV, mo_vreg(rhs), mo_none(), mo_none());
            /* risultato: quoziente in rax, resto in rdx */
            MachPhysReg result_reg = (in->op == IR_DIV) ? PHYS_RAX : PHYS_RDX;
            mfunc_emit(f, MACH_MOV, mo_vreg(dst), mo_phys(result_reg), mo_none());
            break;
        }

        /* ----------------------------------------------------------------
         * Negazione unaria: MOV accetta immediato come src
         * ---------------------------------------------------------------- */
        case IR_NEG: {
            int         dst = operand_to_vreg(&in->dst, &vm, f);
            MachOperand src = operand_to_mach(&in->src1, &vm, f);
            int         tmp = mfunc_new_vreg(f);
            mfunc_emit(f, MACH_MOV, mo_vreg(tmp), src,           mo_none());
            mfunc_emit(f, MACH_NEG, mo_vreg(tmp), mo_none(),     mo_none());
            mfunc_emit(f, MACH_MOV, mo_vreg(dst), mo_vreg(tmp),  mo_none());
            break;
        }

        /* ----------------------------------------------------------------
         * NOT logico: !x = (x == 0)
         * test src,src; sete al; movzx dst,al
         * ---------------------------------------------------------------- */
        case IR_NOT: {
            int dst = operand_to_vreg(&in->dst, &vm, f);
            int src = load_operand(&in->src1, &vm, f);
            mfunc_emit(f, MACH_TEST, mo_vreg(src), mo_vreg(src), mo_none());
            mfunc_emit(f, MACH_SETE, mo_phys(PHYS_AL), mo_none(), mo_none());
            mfunc_emit(f, MACH_MOVSX, mo_vreg(dst), mo_phys(PHYS_AL), mo_none());
            break;
        }

        /* ----------------------------------------------------------------
         * Comparazioni: IR_LT, IR_LE, IR_GT, IR_GE, IR_EQ, IR_NE
         *
         * Non emettiamo subito: salviamo in PendingCmp e aspettiamo
         * l'eventuale IF_FALSE successivo per la fusione CMP+JCC.
         * Se non arriva, materializziamo all'inizio del prossimo ciclo.
         *
         * CMP x86: solo rhs può essere immediato (cmpq $imm, %reg ✓).
         * Se src1 è costante e src2 è vreg, scambiamo e invertiamo op.
         * ---------------------------------------------------------------- */
        case IR_LT: case IR_LE: case IR_GT: case IR_GE:
        case IR_EQ: case IR_NE: {
            int  dst    = operand_to_vreg(&in->dst, &vm, f);
            IROp cmpOp  = in->op;

            MachOperand lhs_mo = operand_to_mach(&in->src1, &vm, f);
            MachOperand rhs_mo = operand_to_mach(&in->src2, &vm, f);

            /* CMP non accetta immediato come primo operando (lhs).
             * Se lhs è immediato e rhs è vreg, scambia + inverti condizione. */
            if (lhs_mo.kind == MO_IMM && rhs_mo.kind != MO_IMM) {
                MachOperand tmp_mo = lhs_mo; lhs_mo = rhs_mo; rhs_mo = tmp_mo;
                cmpOp = flip_cmp(cmpOp);
            }

            /* Se lhs è ancora un immediato (entrambi costanti), materializza
             * lhs in un vreg (caso molto raro post-CP, ma corretto). */
            int lhs_vreg;
            if (lhs_mo.kind == MO_IMM) {
                lhs_vreg = mfunc_new_vreg(f);
                mfunc_emit(f, MACH_MOV, mo_vreg(lhs_vreg), lhs_mo, mo_none());
                lhs_mo = mo_vreg(lhs_vreg);
            }

            /* Ricava l'ID vreg del lhs (già un vreg dopo la normalizzazione) */
            int lhs_id = lhs_mo.vregId;

            /* Salva operandi come vreg per il PendingCmp.
             * rhs_mo può essere MO_IMM o MO_VREG: lo salviamo come campo
             * separato nel PendingCmp esteso. */
            pcmp.active   = 1;
            pcmp.cmpOp    = cmpOp;
            pcmp.src1Vreg = lhs_id;
            /* Per rhs immediato: materializza ora in vreg temporaneo
             * (CMP con immediato viene emesso direttamente nell'emissione). */
            if (rhs_mo.kind == MO_IMM) {
                int rhs_tmp = mfunc_new_vreg(f);
                mfunc_emit(f, MACH_MOV, mo_vreg(rhs_tmp), rhs_mo, mo_none());
                pcmp.src2Vreg = rhs_tmp;
            } else {
                pcmp.src2Vreg = rhs_mo.vregId;
            }
            pcmp.dstVreg  = dst;
            break;
        }

        /* ----------------------------------------------------------------
         * Accesso ad array: LOAD_ARR
         * dst = base[idx]  →  mov rax,[base_addr + idx*8]
         * ---------------------------------------------------------------- */
        case IR_LOAD_ARR: {
            int dst     = operand_to_vreg(&in->dst, &vm, f);
            int base    = operand_to_vreg(&in->src1, &vm, f);
            int idx     = load_operand(&in->src2, &vm, f);
            /* Emetti un LOAD con indirizzamento base+index*8 */
            mfunc_emit(f, MACH_LOAD, mo_vreg(dst),
                       mo_mem(base, idx, 8, 0), mo_none());
            break;
        }

        /* ----------------------------------------------------------------
         * Scrittura ad array: STORE_ARR
         * base[idx] = src  →  mov [base_addr + idx*8], src
         * ---------------------------------------------------------------- */
        case IR_STORE_ARR: {
            int base = operand_to_vreg(&in->dst, &vm, f);
            int idx  = load_operand(&in->src1, &vm, f);
            int src  = load_operand(&in->src2, &vm, f);
            mfunc_emit(f, MACH_STORE,
                       mo_mem(base, idx, 8, 0), mo_vreg(src), mo_none());
            break;
        }

        /* ----------------------------------------------------------------
         * Chiamate di funzione: PARAM + CALL
         *
         * IR_PARAM: accumula argomenti nel buffer param_vregs.
         * IR_CALL: emette MOV verso registri argomento, CALL, sposta rax.
         * ---------------------------------------------------------------- */
        case IR_PARAM: {
            /* pushq $imm è istruzione legale in x86-64: evita vreg intermedio
             * per costanti. Materializziamo comunque in vreg per semplicità
             * del buffer param_vregs (il regalloc gestirà l'immediato). */
            MachOperand src_mo = operand_to_mach(&in->src1, &vm, f);
            int src;
            if (src_mo.kind == MO_IMM) {
                src = mfunc_new_vreg(f);
                mfunc_emit(f, MACH_MOV, mo_vreg(src), src_mo, mo_none());
            } else {
                src = src_mo.vregId;
            }
            if (param_count < MAX_PARAMS)
                param_vregs[param_count++] = src;
            break;
        }

        case IR_CALL: {
            /* Argomenti: primi 6 nei registri, resto sullo stack (push inverso) */
            int n = param_count;

            /* Push argomenti extra (oltre i 6) in ordine inverso */
            for (int k = n - 1; k >= NUM_ARG_REGS; k--)
                mfunc_emit(f, MACH_PUSH, mo_vreg(param_vregs[k]), mo_none(), mo_none());

            /* Carica i primi N≤6 argomenti nei registri */
            int reg_args = (n < NUM_ARG_REGS) ? n : NUM_ARG_REGS;
            for (int k = 0; k < reg_args; k++)
                mfunc_emit(f, MACH_MOV,
                           mo_phys(ARG_REGS[k]), mo_vreg(param_vregs[k]), mo_none());

            /* CALL */
            mfunc_emit(f, MACH_CALL, mo_func(in->src1.data.funcName), mo_none(), mo_none());

            /* Pulisci stack se ci sono argomenti extra */
            int extra = n - NUM_ARG_REGS;
            if (extra > 0) {
                int adj = mfunc_new_vreg(f);
                mfunc_emit(f, MACH_MOV, mo_vreg(adj), mo_imm(extra * 8L), mo_none());
                mfunc_emit(f, MACH_ADD, mo_phys(PHYS_RSP), mo_vreg(adj), mo_none());
            }

            /* Risultato: rax → dst vreg */
            int dst = operand_to_vreg(&in->dst, &vm, f);
            mfunc_emit(f, MACH_MOV, mo_vreg(dst), mo_phys(PHYS_RAX), mo_none());

            param_count = 0;   /* reset buffer argomenti */
            break;
        }

        /* ----------------------------------------------------------------
         * Return
         * ---------------------------------------------------------------- */
        case IR_RETURN: {
            int src = load_operand(&in->src1, &vm, f);
            mfunc_emit(f, MACH_MOV, mo_phys(PHYS_RAX), mo_vreg(src), mo_none());
            mfunc_emit(f, MACH_RET, mo_none(), mo_none(), mo_none());
            break;
        }

        } /* switch */
    } /* for istruzioni */

    /* Se rimane una comparazione pendente non fusa (funzione che finisce
     * con una comparazione senza IF_FALSE dopo), materializzala */
    if (pcmp.active) {
        mfunc_emit(f, MACH_CMP, mo_vreg(pcmp.src1Vreg), mo_vreg(pcmp.src2Vreg), mo_none());
        MachOp setcc = comparison_to_setcc(pcmp.cmpOp);
        mfunc_emit(f, setcc, mo_phys(PHYS_AL), mo_none(), mo_none());
        mfunc_emit(f, MACH_MOVSX, mo_vreg(pcmp.dstVreg), mo_phys(PHYS_AL), mo_none());
    }

    /* Calcola frame size: ogni vreg ottiene uno slot da 8 byte.
     * Arrotondamento a 16 byte per allineamento System V. */
    int raw = f->nextVreg * 8;
    f->frameSize = (raw + 15) & ~15;

    return f;
}

/* =========================================================================
 * isel_select: entry point
 * ========================================================================= */

MachProgram *isel_select(const IRProgram *ir) {
    MachProgram *mp = calloc(1, sizeof(MachProgram));
    mp->capacity  = ir->count ? ir->count : 1;
    mp->functions = malloc((size_t)mp->capacity * sizeof(MachFunction *));

    for (int i = 0; i < ir->count; i++)
        mp->functions[mp->count++] = select_function(ir->functions[i]);

    return mp;
}

/* =========================================================================
 * isel_emit_asm: emissione assembly AT&T x86-64
 * ========================================================================= */

/* Nomi registri fisici (AT&T: prefisso %) */
static const char *phys_name64[] = {
    "%rax", "%rcx", "%rdx", "%rbp", "%rsp",
    "%rdi", "%rsi", "%r8",  "%r9",  "%al"
};

static void emit_operand(const MachOperand *o, FILE *out) {
    switch (o->kind) {
    case MO_NONE:  break;
    case MO_VREG:  fprintf(out, "%%v%d", o->vregId);               break;
    case MO_PHYS:  fprintf(out, "%s", phys_name64[o->physReg]);    break;
    case MO_IMM:   fprintf(out, "$%ld", o->imm);                   break;
    case MO_LABEL: fprintf(out, ".L%d", o->labelId);               break;
    case MO_FUNC:  fprintf(out, "%s", o->func);                    break;
    case MO_MEM:
        if (o->mem.disp)       fprintf(out, "%d", o->mem.disp);
        fprintf(out, "(");
        if (o->mem.baseVreg  >= 0) fprintf(out, "%%v%d", o->mem.baseVreg);
        if (o->mem.indexVreg >= 0) fprintf(out, ",%%v%d,%d",
                                            o->mem.indexVreg, o->mem.scale);
        fprintf(out, ")");
        break;
    case MO_FIMM:  fprintf(out, "$0x%x", (unsigned)(int)o->fimm); break;
    }
}

void isel_emit_asm(const MachProgram *mp, FILE *out) {
    fprintf(out, "\t.text\n");

    for (int fi = 0; fi < mp->count; fi++) {
        const MachFunction *f = mp->functions[fi];

        fprintf(out, "\t.globl %s\n", f->name);
        fprintf(out, "%s:\n", f->name);

        for (int i = 0; i < f->count; i++) {
            const MachInstr *in = &f->instrs[i];

            switch (in->op) {
            case MACH_LABEL:
                fprintf(out, ".L%d:\n", in->dst.labelId);
                continue;

            case MACH_FUNC_BEGIN:
                fprintf(out, "\tpushq\t%%rbp\n");
                fprintf(out, "\tmovq\t%%rsp, %%rbp\n");
                if (f->frameSize > 0)
                    fprintf(out, "\tsubq\t$%d, %%rsp\n", f->frameSize);
                continue;

            case MACH_FUNC_END:
                continue;   /* gestito da RET */

            case MACH_RET:
                fprintf(out, "\tleave\n");
                fprintf(out, "\tret\n");
                continue;

            case MACH_CQO:
                fprintf(out, "\tcqo\n");
                continue;

            case MACH_IDIV:
                fprintf(out, "\tidivq\t");
                emit_operand(&in->dst, out);
                fprintf(out, "\n");
                continue;

            case MACH_NEG:
                fprintf(out, "\tnegq\t");
                emit_operand(&in->dst, out);
                fprintf(out, "\n");
                continue;

            case MACH_NOT:
                fprintf(out, "\tnotq\t");
                emit_operand(&in->dst, out);
                fprintf(out, "\n");
                continue;

            case MACH_PUSH:
                fprintf(out, "\tpushq\t");
                emit_operand(&in->dst, out);
                fprintf(out, "\n");
                continue;

            case MACH_POP:
                fprintf(out, "\tpopq\t");
                emit_operand(&in->dst, out);
                fprintf(out, "\n");
                continue;

            case MACH_CALL:
                fprintf(out, "\tcall\t");
                emit_operand(&in->dst, out);
                fprintf(out, "\n");
                continue;

            /* Istruzioni setcc: operano su %al */
            case MACH_SETE:  fprintf(out, "\tsete\t%%al\n");  continue;
            case MACH_SETNE: fprintf(out, "\tsetne\t%%al\n"); continue;
            case MACH_SETL:  fprintf(out, "\tsetl\t%%al\n");  continue;
            case MACH_SETLE: fprintf(out, "\tsetle\t%%al\n"); continue;
            case MACH_SETG:  fprintf(out, "\tsetg\t%%al\n");  continue;
            case MACH_SETGE: fprintf(out, "\tsetge\t%%al\n"); continue;

            /* Salti */
            case MACH_JMP:
                fprintf(out, "\tjmp\t.L%d\n", in->dst.labelId); continue;
            case MACH_JE:
                fprintf(out, "\tje\t.L%d\n",  in->dst.labelId); continue;
            case MACH_JNE:
                fprintf(out, "\tjne\t.L%d\n", in->dst.labelId); continue;
            case MACH_JL:
                fprintf(out, "\tjl\t.L%d\n",  in->dst.labelId); continue;
            case MACH_JLE:
                fprintf(out, "\tjle\t.L%d\n", in->dst.labelId); continue;
            case MACH_JG:
                fprintf(out, "\tjg\t.L%d\n",  in->dst.labelId); continue;
            case MACH_JGE:
                fprintf(out, "\tjge\t.L%d\n", in->dst.labelId); continue;

            /* Istruzioni a 2 operandi: mnemonico src, dst */
            default: break;
            }

            /* Istruzioni generali con mnemonic */
            const char *mnem = NULL;
            switch (in->op) {
            case MACH_MOV:   mnem = "movq";  break;
            case MACH_MOVSX: mnem = "movsbq"; break;
            case MACH_ADD:   mnem = "addq";  break;
            case MACH_SUB:   mnem = "subq";  break;
            case MACH_IMUL:  mnem = "imulq"; break;
            case MACH_SAL:   mnem = "salq";  break;
            case MACH_XOR:   mnem = "xorq";  break;
            case MACH_CMP:   mnem = "cmpq";  break;
            case MACH_TEST:  mnem = "testq"; break;
            case MACH_LOAD:  mnem = "movq";  break;
            case MACH_STORE: mnem = "movq";  break;
            default: mnem = "???"; break;
            }

            fprintf(out, "\t%s\t", mnem);

            /* AT&T: src prima di dst per istruzioni binarie */
            if (in->op == MACH_LOAD) {
                /* LOAD: src è l'indirizzo MEM, dst è il vreg */
                emit_operand(&in->src1, out);
                fprintf(out, ", ");
                emit_operand(&in->dst, out);
            } else if (in->op == MACH_STORE) {
                /* STORE: src è il vreg, dst è l'indirizzo MEM */
                emit_operand(&in->src1, out);
                fprintf(out, ", ");
                emit_operand(&in->dst, out);
            } else if (in->src2.kind != MO_NONE) {
                /* binaria: src2 src1 dst?  No: per ADD/SUB/ecc. in AT&T:
                 * addq src, dst  (dst += src) → src1=rhs, dst=dst */
                emit_operand(&in->src1, out);
                fprintf(out, ", ");
                emit_operand(&in->dst, out);
            } else if (in->src1.kind != MO_NONE) {
                /* unaria o mov: src, dst */
                emit_operand(&in->src1, out);
                if (in->dst.kind != MO_NONE) {
                    fprintf(out, ", ");
                    emit_operand(&in->dst, out);
                }
            } else {
                emit_operand(&in->dst, out);
            }
            fprintf(out, "\n");
        }

        fprintf(out, "\n");
    }
}

/* =========================================================================
 * mach_free
 * ========================================================================= */

void mach_free(MachProgram *mp) {
    if (!mp) return;
    for (int i = 0; i < mp->count; i++) {
        free(mp->functions[i]->instrs);
        free(mp->functions[i]);
    }
    free(mp->functions);
    free(mp);
}