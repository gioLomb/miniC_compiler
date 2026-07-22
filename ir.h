#ifndef IR_H
#define IR_H

/* Capacita' iniziale dell'array di istruzioni per funzione. Alzata da 16
 * a 64: con il raddoppio geometrico gia' presente in emit(), il numero
 * di realloc per una funzione di media dimensione scende da ~6 a ~3
 * (es. per 500 istruzioni: 64->128->256->512 invece di 16->32->64->128->256->512).
 * Nessuna passata dedicata a pre-contare le istruzioni: si accetta un
 * margine di realloc residuo in cambio di zero costo aggiuntivo e zero
 * campi extra su ASTNode. */
#define IR_INITIAL_CAPACITY 64
/*
 * Generazione di IR lineare (three-address code) a partire dall'AST gia'
 * validato da semantic_check() e, opzionalmente, semplificato da
 * optimize_ast() (entrambi devono essere gia' girati: ir_generate() non
 * ripete alcuna verifica semantica, assume l'AST corretto).
 *
 * Identita' delle variabili e shadowing
 * --------------------------------------
 * ir_generate() non tocca MAI la symbol table: legge direttamente
 * node->scopeLevel/node->offset, stampigliati da semantic_check() su ogni
 * ND_ID/ND_ARRAY_ACCESS/ND_VAR_DECL/ND_PARAM (vedi ast.h). Quella coppia
 * identifica univocamente la variabile anche in presenza di shadowing
 * (due dichiarazioni con lo stesso nome, a livelli diversi, ricevono
 * coordinate diverse) - e' il motivo per cui questo modulo dipende solo
 * da ast.h, non da symbol_table.h: O(1) per riferimento, zero lookup.
 *
 * Modello di istruzione: quadruple (IROp, dst, src1, src2). Controllo di
 * flusso via salti/etichette esplicite (IR_GOTO/IR_IF_FALSE/IR_LABEL);
 * && e || sono tradotti a corto-circuito (il secondo operando NON viene
 * valutato se il risultato e' gia' determinato dal primo - rilevante per
 * i suoi eventuali effetti collaterali, es. "f() && g()").
 */

#include "parser/ast.h"

typedef enum {
    IR_ADD, IR_SUB, IR_MUL, IR_DIV, IR_MOD,
    IR_NEG,                     /* dst = -src1 */
    IR_NOT,                     /* dst = !src1 (booleano 0/1) */
    IR_LT, IR_LE, IR_GT, IR_GE, IR_EQ, IR_NE,   /* dst = src1 OP src2, risultato booleano 0/1 */
    IR_ASSIGN,                  /* dst = src1 */
    IR_LOAD_ARR,                /* dst = src1[src2]   (src1 = base array, src2 = indice) */
    IR_STORE_ARR,                /* dst[src1] = src2   (dst = base array, src1 = indice) */
    IR_PARAM,                   /* prepara src1 come prossimo argomento di una IR_CALL */
    IR_CALL,                    /* dst = call src1(src2 argomenti gia' preparati con IR_PARAM) */
    IR_RETURN,                  /* return src1 */
    IR_GOTO,                    /* salta incondizionatamente a dst (un'etichetta) */
    IR_IF_FALSE,                /* se src1 e' falso (0), salta a dst (un'etichetta) */
    IR_LABEL                    /* segna la posizione dell'etichetta dst */
} IROp;

typedef enum {
    OPND_NONE,          /* operando assente/non usato in questa istruzione */
    OPND_TEMP,          /* temporaneo generato dal compilatore: t0, t1, ... */
    OPND_VAR,           /* variabile sorgente: identita' = (level, offset) */
    OPND_CONST_INT,
    OPND_CONST_FLOAT,
    OPND_LABEL,         /* L0, L1, ... - bersaglio di IR_GOTO/IR_IF_FALSE/IR_LABEL */
    OPND_FUNC           /* nome di funzione, solo come src1 di IR_CALL */
} OperandKind;

typedef struct {
    OperandKind kind;
    union {
        int tempId;
        struct {
            int level;
            int offset;
            const char *sourceName;  /* non-owning: punta a node->text nell'AST, solo per stampa/debug */
        } var;
        long intVal;
        double floatVal;
        int labelId;
        const char *funcName;        /* non-owning: punta a node->text nell'AST */
    } as;
} Operand;

typedef struct {
    IROp op;
    Operand dst, src1, src2;
} IRInstr;

/* Basic block del CFG: range [start, end) in instrs, e i suoi successori
 * (indici di blocco, -1 se assente: -1 in succ[0]/succ[1] compare per un
 * IR_RETURN, o per un IR_GOTO/IR_IF_FALSE il cui bersaglio e' comunque
 * l'ultimo blocco della funzione). predCount conta gli ARCHI entranti,
 * non i blocchi predecessori distinti: un IR_IF_FALSE il cui fallthrough
 * e bersaglio coincidono (es. "if (c) {}" con ramo then vuoto) conta 2,
 * pur avendo un solo blocco di origine - e' corretto cosi' (il blocco
 * risultante e' comunque raggiunto da due archi runtime distinti, mai
 * entrambi nella stessa esecuzione), semplicemente non e' idoneo alla
 * fusione via singolo predecessore che fa svn.c, e viene trattato come
 * punto di confluenza. Nessuna perdita di correttezza, solo un'occasione
 * di propagazione mancata in un caso raro. */
typedef struct {
    int start, end;
    int succ[2];
    int predCount;
} IRBlock;

typedef struct {
    char *name;          /* copia propria, indipendente dall'AST */
    IRInstr *instrs;
    int count, capacity;

    IRBlock *blocks;      /* CFG, costruito incrementalmente in emit() (vedi ir.c) */
    int blockCount, blockCapacity;

    /* ---- scratch di costruzione: significativi SOLO durante irFunction(),
     * azzerati/liberati da resolveCFG() prima che la funzione ritorni.
     * Nessun chiamante esterno a ir.c deve leggerli. */
    int curBlockStart;
    int labelBase;
    int *labelToBlock;
    int labelToBlockCap;
} IRFunction;

typedef struct {
    IRFunction **functions;
    int count, capacity;
} IRProgram;

/* Traduce l'intero programma (tutti i ND_FUNC_DECL top-level). Le
 * dichiarazioni di variabili globali (ND_VAR_DECL a livello di ND_PROGRAM)
 * non producono IR in questa prima versione: l'inizializzazione di dati
 * statici e' un problema diverso (layout di memoria globale), non ancora
 * affrontato in questa fase del progetto. */
IRProgram *ir_generate(ASTNode *program);

/* Stampa il programma IR in forma leggibile su stdout (una funzione alla
 * volta, un'istruzione per riga) - utile per debug e per confrontare
 * l'output prima/dopo eventuali ottimizzazioni sull'IR lineare. */
void ir_print(const IRProgram *prog);

/* Libera ricorsivamente IRProgram, ogni IRFunction e i relativi array di
 * istruzioni. Non tocca l'AST (gli Operand di tipo OPND_VAR/OPND_FUNC
 * puntano dentro node->text, non ne possiedono una copia). */
void ir_free(IRProgram *prog);

#endif
