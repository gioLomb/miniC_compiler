#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ir.h"
#include "svn.h"

/* Contatori globali per temporanei ed etichette: a differenza dell'offset
 * delle variabili (che DEVE ripartire da zero per ogni scope, per questo
 * viene letto da scope->table->size in fase semantica), t0/t1/... e
 * L0/L1/... sono per costruzione un'unica sequenza per l'intero programma
 * - non c'e' alcun contesto rispetto a cui "azzerarli", quindi un
 * contatore semplice, reinizializzato una sola volta all'inizio di
 * ir_generate(), e' corretto cosi' com'e' (nessun riuso di temporanei per
 * ora: ogni sotto-espressione ne ottiene uno nuovo). */
static int nextTemp;
static int nextLabel;

static Operand mkTemp(void) {
    Operand o; o.kind = OPND_TEMP; o.as.tempId = nextTemp++; return o;
}
static Operand mkLabel(void) {
    Operand o; o.kind = OPND_LABEL; o.as.labelId = nextLabel++; return o;
}
static Operand mkVar(const ASTNode *node) {
    Operand o;
    o.kind = OPND_VAR;
    o.as.var.level = node->scopeLevel;
    o.as.var.offset = node->offset;
    o.as.var.sourceName = node->text;
    return o;
}
static Operand mkConstInt(long v) {
    Operand o; o.kind = OPND_CONST_INT; o.as.intVal = v; return o;
}
static Operand mkConstFloat(double v) {
    Operand o; o.kind = OPND_CONST_FLOAT; o.as.floatVal = v; return o;
}
static Operand mkFunc(const char *name) {
    Operand o; o.kind = OPND_FUNC; o.as.funcName = name; return o;
}
static Operand noOperand(void) {
    Operand o; o.kind = OPND_NONE; return o;
}

/* ---- Costruzione live del CFG -----------------------------------------
 *
 * I confini di blocco vengono rilevati nell'istante stesso in cui
 * un'istruzione viene scritta, dentro emit() - l'unico punto per cui
 * passa OGNI istruzione emessa, da qualunque funzione del traduttore
 * (irExpr/irExprInto/irStmt/irJumpIfFalse/irShortCircuitInto...).
 * Nessuna di quelle funzioni viene toccata: bastano emit() stessa e la
 * chiusura finale in irFunction()/resolveCFG().
 *
 * Vantaggio rispetto a una scansione separata di f->instrs dopo la
 * traduzione: ogni istruzione viene toccata una volta sola in totale
 * (qui, mentre viene comunque scritta), non due. */

static int isTerminator(IROp op) {
    return op == IR_GOTO || op == IR_IF_FALSE || op == IR_RETURN;
}

static void closeBlock(IRFunction *f, int start, int end) {
    if (end <= start) return;   /* nessun contenuto pendente: nulla da chiudere */
    if (f->blockCount == f->blockCapacity) {
        f->blockCapacity = f->blockCapacity ? f->blockCapacity * 2 : 16;
        f->blocks = realloc(f->blocks, (size_t) f->blockCapacity * sizeof(IRBlock));
    }
    IRBlock *b = &f->blocks[f->blockCount++];
    b->start = start;
    b->end = end;
    b->succ[0] = b->succ[1] = -1;
    b->predCount = 0;
}

/* Registra che l'etichetta 'labelId' apre il blocco che AVRA' indice
 * 'futureBlockIdx' quando verra' infine chiuso. Indicizzata da
 * (labelId - f->labelBase): nextLabel e' un contatore globale al
 * programma (vedi sopra), ma le etichette di una singola funzione sono
 * comunque un intervallo contiguo (la traduzione avviene una funzione
 * alla volta) - un array a crescita raddoppiata basta, O(1) per accesso,
 * senza l'overhead di una hash table per uno spazio di chiavi denso e
 * sotto controllo del compilatore. */
static void registerLabel(IRFunction *f, int labelId, int futureBlockIdx) {
    int idx = labelId - f->labelBase;
    if (idx >= f->labelToBlockCap) {
        int newCap = f->labelToBlockCap ? f->labelToBlockCap * 2 : 8;
        while (newCap <= idx) newCap *= 2;
        f->labelToBlock = realloc(f->labelToBlock, (size_t) newCap * sizeof(int));
        for (int i = f->labelToBlockCap; i < newCap; i++) f->labelToBlock[i] = -1;
        f->labelToBlockCap = newCap;
    }
    f->labelToBlock[idx] = futureBlockIdx;
}

static void emit(IRFunction *f, IROp op, Operand dst, Operand src1, Operand src2) {
    if (f->count == f->capacity) {
        f->capacity = f->capacity ? f->capacity * 2 : 16;
        f->instrs = realloc(f->instrs, (size_t) f->capacity * sizeof(IRInstr));
    }
    int idx = f->count;

    /* Una IR_LABEL apre sempre un nuovo blocco: se c'e' contenuto
       pendente dal blocco corrente, chiudilo prima di scrivere questa
       istruzione (la label appartiene al blocco NUOVO, non a quello che
       la precede). */
    if (op == IR_LABEL && idx > f->curBlockStart) {
        closeBlock(f, f->curBlockStart, idx);
        f->curBlockStart = idx;
    }

    f->instrs[idx].op = op;
    f->instrs[idx].dst = dst;
    f->instrs[idx].src1 = src1;
    f->instrs[idx].src2 = src2;
    f->count++;

    if (op == IR_LABEL) {
        /* Il blocco che si sta aprendo ora (f->curBlockStart == idx) non
           e' ancora stato spinto in f->blocks: ricevera' l'indice
           f->blockCount quando verra' infine chiuso, perche' i blocchi
           sono sempre spinti in ordine crescente e nessuno e' stato
           ancora spinto per questo. */
        registerLabel(f, dst.as.labelId, f->blockCount);
    }
    if (isTerminator(op)) {
        closeBlock(f, f->curBlockStart, f->count);
        f->curBlockStart = f->count;
    }
}

/* Scorciatoie per le tre istruzioni di controllo di flusso, cosi' la
 * convenzione "l'etichetta viaggia in dst" (vedi ir.h) e' rispettata in
 * un punto solo invece di doverla ricordare ad ogni emit() manuale. */
static void emitGoto(IRFunction *f, Operand label) {
    emit(f, IR_GOTO, label, noOperand(), noOperand());
}
static void emitIfFalse(IRFunction *f, Operand cond, Operand label) {
    emit(f, IR_IF_FALSE, label, cond, noOperand());
}
static void emitLabel(IRFunction *f, Operand label) {
    emit(f, IR_LABEL, label, noOperand(), noOperand());
}

static IROp binopToIROp(const char *op) {
    if (strcmp(op, "+") == 0)  return IR_ADD;
    if (strcmp(op, "-") == 0)  return IR_SUB;
    if (strcmp(op, "*") == 0)  return IR_MUL;
    if (strcmp(op, "/") == 0)  return IR_DIV;
    if (strcmp(op, "%") == 0)  return IR_MOD;
    if (strcmp(op, "<") == 0)  return IR_LT;
    if (strcmp(op, "<=") == 0) return IR_LE;
    if (strcmp(op, ">") == 0)  return IR_GT;
    if (strcmp(op, ">=") == 0) return IR_GE;
    if (strcmp(op, "==") == 0) return IR_EQ;
    if (strcmp(op, "!=") == 0) return IR_NE;
    return IR_ADD;   /* irraggiungibile: semantic_check ha gia' validato l'operatore */
}

static Operand irExpr(ASTNode *expr, IRFunction *out);
static Operand irExprInto(ASTNode *expr, IRFunction *out, Operand dest);
static void irJumpIfFalse(ASTNode *cond, IRFunction *out, Operand falseLbl);
static void irJumpIfTrue(ASTNode *cond, IRFunction *out, Operand trueLbl);

/*
 * ---- Codice di salto per condizioni a corto-circuito (if/while) -------
 *
 * Traduzione CLASSICA "a salti" di un'espressione booleana usata in un
 * contesto di controllo (condizione di if/while) - schema standard da
 * compilatori (Aho/Ullman): invece di calcolare && / || come un VALORE
 * 0/1 e poi ricontrollarlo con un'altra IF_FALSE (come farebbe, e come
 * comunque fa, la traduzione "a valore" usata quando il risultato booleano
 * serve davvero come dato, es. "r = a && b;"), qui traduciamo l'intera
 * espressione DIRETTAMENTE in salti verso l'etichetta che il chiamante
 * gia' possiede (l'uscita del while, il ramo else dell'if) - senza mai
 * materializzare un valore intermedio, senza variabile "risultato", senza
 * blocchi ne' etichette in piu' del necessario.
 *
 * irJumpIfFalse(cond, falseLbl): salta a 'falseLbl' se 'cond' e' falsa,
 * altrimenti prosegue (fallthrough) - usata per "entra nel corpo solo se
 * la condizione e' vera".
 * irJumpIfTrue(cond, trueLbl): duale, salta se 'cond' e' vera - serve
 * internamente per tradurre correttamente il ramo sinistro di un '||'
 * dentro irJumpIfFalse (vedi sotto).
 *
 * Le due funzioni si richiamano a vicenda seguendo le identita' logiche:
 *   (a && b) e' falsa  <=>  a e' falsa OPPURE b e' falsa
 *   (a || b) e' vera   <=>  a e' vera  OPPURE b e' vera
 *   !a e' falsa        <=>  a e' vera
 * Per ogni altro nodo (un confronto, una variabile, una chiamata...) non
 * c'e' struttura di salto da sfruttare: si calcola il valore con la
 * traduzione ordinaria (irExpr, che per un confronto usa comunque un solo
 * IR_LT/IR_LE/... - nessun'altra riduzione possibile a quel livello) e si
 * traduce con un singolo salto condizionale, esattamente come oggi.
 */

static void irJumpIfFalse(ASTNode *cond, IRFunction *out, Operand falseLbl) {
    if (cond->kind == ND_BINOP && strcmp(cond->text, "&&") == 0) {
        /* a && b e' falsa se a e' falsa (salta subito) O se b e' falsa
           (valutata solo se a era vera - corto-circuito) */
        irJumpIfFalse(cond->children[0], out, falseLbl);
        irJumpIfFalse(cond->children[1], out, falseLbl);
        return;
    }
    if (cond->kind == ND_BINOP && strcmp(cond->text, "||") == 0) {
        /* a || b e' falsa solo se ENTRAMBE sono false: se a e' vera,
           salta oltre il controllo di b (non e' falsa, quindi non si
           salta a falseLbl); altrimenti (a falsa) il verdetto dipende
           solo da b. */
        Operand skipLbl = mkLabel();
        irJumpIfTrue(cond->children[0], out, skipLbl);
        irJumpIfFalse(cond->children[1], out, falseLbl);
        emitLabel(out, skipLbl);
        return;
    }
    if (cond->kind == ND_UNARY && strcmp(cond->text, "!") == 0) {
        /* !a e' falsa esattamente quando a e' vera */
        irJumpIfTrue(cond->children[0], out, falseLbl);
        return;
    }
    Operand v = irExpr(cond, out);
    emitIfFalse(out, v, falseLbl);
}

static void irJumpIfTrue(ASTNode *cond, IRFunction *out, Operand trueLbl) {
    if (cond->kind == ND_BINOP && strcmp(cond->text, "&&") == 0) {
        /* a && b e' vera solo se ENTRAMBE lo sono: se a e' falsa, salta
           oltre (non puo' essere vera); altrimenti dipende da b. */
        Operand skipLbl = mkLabel();
        irJumpIfFalse(cond->children[0], out, skipLbl);
        irJumpIfTrue(cond->children[1], out, trueLbl);
        emitLabel(out, skipLbl);
        return;
    }
    if (cond->kind == ND_BINOP && strcmp(cond->text, "||") == 0) {
        /* a || b e' vera se a e' vera (salta subito) O se b lo e' */
        irJumpIfTrue(cond->children[0], out, trueLbl);
        irJumpIfTrue(cond->children[1], out, trueLbl);
        return;
    }
    if (cond->kind == ND_UNARY && strcmp(cond->text, "!") == 0) {
        irJumpIfFalse(cond->children[0], out, trueLbl);
        return;
    }
    /* Caso base: l'unico primitivo di salto che abbiamo e' "salta se
       falso", quindi per "salta se vero" serve un salto in piu' (salta
       oltre se falso, altrimenti vai a trueLbl) - inevitabile con questo
       set di istruzioni, e comunque il caso base e' raro (si attiva solo
       per un '!'/'||' il cui operando e' a sua volta un'espressione
       semplice, non un'altra catena &&/||/!). */
    Operand v = irExpr(cond, out);
    Operand skipLbl = mkLabel();
    emitIfFalse(out, v, skipLbl);
    emitGoto(out, trueLbl);
    emitLabel(out, skipLbl);
}

/*
 * ---- Traduzione "a valore" di &&/|| ------------------------------------
 * Usata SOLO quando il risultato booleano serve davvero come dato (es.
 * "r = a && b;", non come condizione di if/while - li' si passa da
 * irJumpIfFalse sopra, che non materializza mai un valore). 'dest' e'
 * l'operando (variabile o temporaneo) in cui scrivere 0/1.
 */
static Operand irShortCircuitInto(ASTNode *expr, IRFunction *out, Operand dest) {
    Operand endLbl = mkLabel();
    Operand falseLbl = mkLabel();

    irJumpIfFalse(expr, out, falseLbl);
    emit(out, IR_ASSIGN, dest, mkConstInt(1), noOperand());
    emitGoto(out, endLbl);
    emitLabel(out, falseLbl);
    emit(out, IR_ASSIGN, dest, mkConstInt(0), noOperand());
    emitLabel(out, endLbl);
    return dest;
}
static Operand irShortCircuit(ASTNode *expr, IRFunction *out) {
    return irShortCircuitInto(expr, out, mkTemp());
}

static Operand irAssign(ASTNode *expr, IRFunction *out) {
    ASTNode *lvalue = expr->children[0];

    if (lvalue->kind == ND_ID) {
        /* Traduce il RHS direttamente nella variabile di destinazione,
           invece che in un temporaneo seguito da una copia (vedi
           irExprInto) - "cane = a+b" produce "cane = a+b", non
           "t = a+b; cane = t;". */
        return irExprInto(expr->children[1], out, mkVar(lvalue));
    }
    /* ND_ARRAY_ACCESS: arr[idx] = rhs - IR_STORE_ARR accetta comunque un
       operando qualsiasi (anche un temporaneo) come valore da scrivere,
       quindi qui non c'e' nessuna copia ridondante da eliminare: il
       valore calcolato da irExpr finisce gia' direttamente nell'istruzione
       di store, non serve un passaggio intermedio in piu'. */
    Operand idx = irExpr(lvalue->children[0], out);
    Operand base = mkVar(lvalue);
    Operand rhs = irExpr(expr->children[1], out);
    emit(out, IR_STORE_ARR, base, idx, rhs);
    return rhs;   /* il valore di un assegnamento e' il valore assegnato */
}

static Operand irCall(ASTNode *expr, IRFunction *out) {
    for (int i = 0; i < expr->nchildren; i++) {
        Operand arg = irExpr(expr->children[i], out);
        emit(out, IR_PARAM, noOperand(), arg, noOperand());
    }
    Operand result = mkTemp();
    emit(out, IR_CALL, result, mkFunc(expr->text), mkConstInt(expr->nchildren));
    return result;
}

static Operand irExpr(ASTNode *expr, IRFunction *out) {
    switch (expr->kind) {

    case ND_NUM_INT:
        return mkConstInt(atol(expr->text));

    case ND_NUM_FLOAT:
        return mkConstFloat(atof(expr->text));

    case ND_ID:
        return mkVar(expr);

    case ND_ARRAY_ACCESS: {
        Operand idx = irExpr(expr->children[0], out);
        Operand base = mkVar(expr);
        Operand t = mkTemp();
        emit(out, IR_LOAD_ARR, t, base, idx);
        return t;
    }

    case ND_UNARY: {
        Operand v = irExpr(expr->children[0], out);
        Operand t = mkTemp();
        emit(out, strcmp(expr->text, "!") == 0 ? IR_NOT : IR_NEG, t, v, noOperand());
        return t;
    }

    case ND_BINOP:
        if (strcmp(expr->text, "&&") == 0 || strcmp(expr->text, "||") == 0) {
            return irShortCircuit(expr, out);
        } else {
            Operand lhs = irExpr(expr->children[0], out);
            Operand rhs = irExpr(expr->children[1], out);
            Operand t = mkTemp();
            emit(out, binopToIROp(expr->text), t, lhs, rhs);
            return t;
        }

    case ND_ASSIGN:
        return irAssign(expr, out);

    case ND_CALL:
        return irCall(expr, out);

    default:
        /* non dovrebbe capitare: semantic_check ha gia' validato l'AST */
        return noOperand();
    }
}

/*
 * Come irExpr, ma per l'espressione al livello piu' ESTERNO di un
 * assegnamento (RHS di ND_ASSIGN su una variabile semplice, o
 * inizializzatore scalare di ND_VAR_DECL): scrive il risultato
 * DIRETTAMENTE in 'dest' invece che in un temporaneo, eliminando la copia
 * finale "temp -> variabile" che altrimenti comparirebbe sempre. Le
 * sotto-espressioni annidate continuano a passare da irExpr()/un
 * temporaneo come prima - solo il nodo esterno, quello il cui risultato
 * e' gia' destinato a una variabile con nome, salta il temporaneo.
 */
static Operand irExprInto(ASTNode *expr, IRFunction *out, Operand dest) {
    switch (expr->kind) {

    case ND_NUM_INT:
        emit(out, IR_ASSIGN, dest, mkConstInt(atol(expr->text)), noOperand());
        return dest;

    case ND_NUM_FLOAT:
        emit(out, IR_ASSIGN, dest, mkConstFloat(atof(expr->text)), noOperand());
        return dest;

    case ND_ID:
        emit(out, IR_ASSIGN, dest, mkVar(expr), noOperand());
        return dest;

    case ND_ARRAY_ACCESS: {
        Operand idx = irExpr(expr->children[0], out);
        Operand base = mkVar(expr);
        emit(out, IR_LOAD_ARR, dest, base, idx);
        return dest;
    }

    case ND_UNARY: {
        Operand v = irExpr(expr->children[0], out);
        emit(out, strcmp(expr->text, "!") == 0 ? IR_NOT : IR_NEG, dest, v, noOperand());
        return dest;
    }

    case ND_BINOP:
        if (strcmp(expr->text, "&&") == 0 || strcmp(expr->text, "||") == 0) {
            return irShortCircuitInto(expr, out, dest);
        } else {
            Operand lhs = irExpr(expr->children[0], out);
            Operand rhs = irExpr(expr->children[1], out);
            emit(out, binopToIROp(expr->text), dest, lhs, rhs);
            return dest;
        }

    case ND_CALL: {
        for (int i = 0; i < expr->nchildren; i++) {
            Operand arg = irExpr(expr->children[i], out);
            emit(out, IR_PARAM, noOperand(), arg, noOperand());
        }
        emit(out, IR_CALL, dest, mkFunc(expr->text), mkConstInt(expr->nchildren));
        return dest;
    }

    case ND_ASSIGN: {
        /* "cane = (i = 5)": l'assegnamento annidato scrive gia' la SUA
           variabile (i); cane deve comunque ricevere una copia del valore
           - due variabili distinte richiedono davvero due scritture,
           non e' una copia ridondante come quella che eliminiamo sopra. */
        Operand inner = irAssign(expr, out);
        emit(out, IR_ASSIGN, dest, inner, noOperand());
        return dest;
    }

    default:
        return noOperand();
    }
}

static void irStmt(ASTNode *stmt, IRFunction *out) {
    if (!stmt) return;

    switch (stmt->kind) {

    case ND_BLOCK:
        /* nessuno scope da aprire qui: node->scopeLevel/offset sono gia'
           stati risolti da semantic_check, basta iterare i figli */
        for (int i = 0; i < stmt->nchildren; i++) {
            irStmt(stmt->children[i], out);
        }
        break;

    case ND_VAR_DECL: {
        if (stmt->nchildren == 0) break;   /* nessun inizializzatore: nulla da emettere */
        if (stmt->nchildren == 1) {
            /* scalare: direttamente nella variabile, niente temporaneo intermedio */
            irExprInto(stmt->children[0], out, mkVar(stmt));
        } else {
            /* array: un figlio per elemento, in ordine */
            for (int i = 0; i < stmt->nchildren; i++) {
                Operand v = irExpr(stmt->children[i], out);
                emit(out, IR_STORE_ARR, mkVar(stmt), mkConstInt(i), v);
            }
        }
        break;
    }

    case ND_EXPR_STMT:
        irExpr(stmt->children[0], out);   /* scarta il risultato, tiene gli effetti collaterali */
        break;

    case ND_IF: {
        Operand elseLbl = mkLabel();
        irJumpIfFalse(stmt->children[0], out, elseLbl);
        irStmt(stmt->children[1], out);
        if (stmt->nchildren > 2) {
            Operand endLbl = mkLabel();
            emitGoto(out, endLbl);
            emitLabel(out, elseLbl);
            irStmt(stmt->children[2], out);
            emitLabel(out, endLbl);
        } else {
            emitLabel(out, elseLbl);
        }
        break;
    }

    case ND_WHILE: {
        Operand startLbl = mkLabel();
        Operand endLbl = mkLabel();
        emitLabel(out, startLbl);
        irJumpIfFalse(stmt->children[0], out, endLbl);
        irStmt(stmt->children[1], out);
        emitGoto(out, startLbl);
        emitLabel(out, endLbl);
        break;
    }

    case ND_RETURN: {
        Operand v = irExpr(stmt->children[0], out);   /* sempre presente: grammatica non ammette 'return;' nudo */
        emit(out, IR_RETURN, noOperand(), v, noOperand());
        break;
    }

    default:
        /* ND_ERROR e altro: ignora, come in checkStmt */
        break;
    }
}

/* Chiude l'eventuale blocco finale pendente (l'ultima istruzione della
 * funzione non e' detto sia un terminatore: es. un corpo il cui ultimo
 * statement non è una IR_RETURN esplicita in coda), poi risolve
 * succ[0]/succ[1]/predCount per ogni blocco usando la mappa etichette
 * gia' costruita live da emit(). Stessa logica di risoluzione salti che
 * prima viveva in buildBlocks() dentro svn.c, solo senza doverli
 * ricostruire da zero: i blocchi esistono gia'. */
static void resolveCFG(IRFunction *f) {
    if (f->curBlockStart < f->count) {
        closeBlock(f, f->curBlockStart, f->count);
        f->curBlockStart = f->count;
    }

    for (int b = 0; b < f->blockCount; b++) {
        int last = f->blocks[b].end - 1;
        IROp op = f->instrs[last].op;
        if (op == IR_GOTO) {
            int lbl = f->instrs[last].dst.as.labelId - f->labelBase;
            f->blocks[b].succ[0] = (lbl >= 0 && lbl < f->labelToBlockCap) ? f->labelToBlock[lbl] : -1;
        } else if (op == IR_IF_FALSE) {
            f->blocks[b].succ[0] = (b + 1 < f->blockCount) ? b + 1 : -1;   /* fallthrough */
            int lbl = f->instrs[last].dst.as.labelId - f->labelBase;
            f->blocks[b].succ[1] = (lbl >= 0 && lbl < f->labelToBlockCap) ? f->labelToBlock[lbl] : -1;
        } else if (op != IR_RETURN) {
            f->blocks[b].succ[0] = (b + 1 < f->blockCount) ? b + 1 : -1;   /* fallthrough semplice */
        }
    }

    for (int b = 0; b < f->blockCount; b++) {
        for (int k = 0; k < 2; k++) {
            int s = f->blocks[b].succ[k];
            if (s >= 0) f->blocks[s].predCount++;
        }
    }

    if (f->blockCount > 0) {
        f->blocks[0].predCount++;
    }

    free(f->labelToBlock);
    f->labelToBlock = NULL;
    f->labelToBlockCap = 0;
}

static IRFunction *irFunction(ASTNode *decl) {
    /* decl->text = "tipo nome" (vedi symtab_parse_decl_text); qui ci
       interessa solo il nome, in coda dopo l'ultimo spazio - stessa
       convenzione gia' usata dal resto del progetto per i decl-text. */
    const char *space = strrchr(decl->text, ' ');
    const char *name = space ? space + 1 : decl->text;

    IRFunction *f = calloc(1, sizeof(IRFunction));
    f->name = strdup(name);
    f->labelBase = nextLabel;   /* le label di questa funzione partono da qui: nextLabel
                                    e' un contatore unico per l'intero programma, ma dentro
                                    una singola funzione forma comunque un intervallo contiguo */

    ASTNode *body = decl->children[decl->nchildren - 1];   /* ND_BLOCK, ultimo figlio */
    irStmt(body, f);

    resolveCFG(f);
    svn_optimize(f);
    return f;
}

static void programPush(IRProgram *prog, IRFunction *f) {
    if (prog->count == prog->capacity) {
        prog->capacity = prog->capacity ? prog->capacity * 2 : 8;
        prog->functions = realloc(prog->functions, (size_t) prog->capacity * sizeof(IRFunction *));
    }
    prog->functions[prog->count++] = f;
}

IRProgram *ir_generate(ASTNode *program) {
    nextTemp = 0;
    nextLabel = 0;

    IRProgram *prog = calloc(1, sizeof(IRProgram));

    for (int i = 0; i < program->nchildren; i++) {
        ASTNode *decl = program->children[i];
        if (decl->kind != ND_FUNC_DECL) continue;   /* le globali non producono IR (vedi ir.h) */
        programPush(prog, irFunction(decl));
    }
    return prog;
}

/* ---- Stampa --------------------------------------------------------- */

static void printOperand(const Operand *o) {
    switch (o->kind) {
    case OPND_NONE:        break;
    case OPND_TEMP:        printf("t%d", o->as.tempId); break;
    case OPND_VAR:
        printf("v%d.%d", o->as.var.level, o->as.var.offset);
        if (o->as.var.sourceName) printf("/*%s*/", o->as.var.sourceName);
        break;
    case OPND_CONST_INT:   printf("%ld", o->as.intVal); break;
    case OPND_CONST_FLOAT: printf("%g", o->as.floatVal); break;
    case OPND_LABEL:       printf("L%d", o->as.labelId); break;
    case OPND_FUNC:        printf("%s", o->as.funcName); break;
    }
}

static const char *opMnemonic(IROp op) {
    switch (op) {
    case IR_ADD: return "+";  case IR_SUB: return "-";
    case IR_MUL: return "*";  case IR_DIV: return "/"; case IR_MOD: return "%";
    case IR_LT: return "<";   case IR_LE: return "<="; case IR_GT: return ">"; case IR_GE: return ">=";
    case IR_EQ: return "=="; case IR_NE: return "!=";
    default: return "?";
    }
}

static void printInstr(const IRInstr *in) {
    switch (in->op) {
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_LT: case IR_LE: case IR_GT: case IR_GE: case IR_EQ: case IR_NE:
        printf("    "); printOperand(&in->dst); printf(" = ");
        printOperand(&in->src1); printf(" %s ", opMnemonic(in->op)); printOperand(&in->src2);
        break;
    case IR_NEG:
        printf("    "); printOperand(&in->dst); printf(" = -"); printOperand(&in->src1);
        break;
    case IR_NOT:
        printf("    "); printOperand(&in->dst); printf(" = !"); printOperand(&in->src1);
        break;
    case IR_ASSIGN:
        printf("    "); printOperand(&in->dst); printf(" = "); printOperand(&in->src1);
        break;
    case IR_LOAD_ARR:
        printf("    "); printOperand(&in->dst); printf(" = "); printOperand(&in->src1);
        printf("["); printOperand(&in->src2); printf("]");
        break;
    case IR_STORE_ARR:
        printf("    "); printOperand(&in->dst); printf("["); printOperand(&in->src1); printf("] = ");
        printOperand(&in->src2);
        break;
    case IR_PARAM:
        printf("    param "); printOperand(&in->src1);
        break;
    case IR_CALL:
        printf("    "); printOperand(&in->dst); printf(" = call "); printOperand(&in->src1);
        printf(", "); printOperand(&in->src2);
        break;
    case IR_RETURN:
        printf("    return "); printOperand(&in->src1);
        break;
    case IR_GOTO:
        printf("    goto "); printOperand(&in->dst);
        break;
    case IR_IF_FALSE:
        printf("    if_false "); printOperand(&in->src1); printf(" goto "); printOperand(&in->dst);
        break;
    case IR_LABEL:
        printOperand(&in->dst); printf(":");
        break;
    }
    printf("\n");
}

void ir_print(const IRProgram *prog) {
    for (int i = 0; i < prog->count; i++) {
        IRFunction *f = prog->functions[i];
        printf("funzione %s:\n", f->name);
        for (int j = 0; j < f->count; j++) {
            printInstr(&f->instrs[j]);
        }
        printf("\n");
    }
}

void ir_free(IRProgram *prog) {
    if (!prog) return;
    for (int i = 0; i < prog->count; i++) {
        free(prog->functions[i]->name);
        free(prog->functions[i]->instrs);
        free(prog->functions[i]->blocks);
        free(prog->functions[i]->labelToBlock);   /* normalmente gia' NULL (liberato in resolveCFG) */
        free(prog->functions[i]);
    }
    free(prog->functions);
    free(prog);
}