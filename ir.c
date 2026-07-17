#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ir.h"

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

static void emit(IRFunction *f, IROp op, Operand dst, Operand src1, Operand src2) {
    if (f->count == f->capacity) {
        f->capacity = f->capacity ? f->capacity * 2 : IR_INITIAL_CAPACITY;
        f->instrs = realloc(f->instrs, (size_t) f->capacity * sizeof(IRInstr));
    }
    f->instrs[f->count].op = op;
    f->instrs[f->count].dst = dst;
    f->instrs[f->count].src1 = src1;
    f->instrs[f->count].src2 = src2;
    f->count++;
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

/* && e || a corto-circuito: il secondo operando si valuta solo se il
 * primo non basta gia' a determinare il risultato. Un solo tipo di salto
 * condizionale (IR_IF_FALSE) e' sufficiente per entrambi gli operatori -
 * non serve introdurre anche un IR_IF_TRUE. */
static Operand irShortCircuit(ASTNode *expr, IRFunction *out) {
    Operand result = mkTemp();
    Operand lhs = irExpr(expr->children[0], out);
    Operand endLbl = mkLabel();

    if (strcmp(expr->text, "&&") == 0) {
        Operand falseLbl = mkLabel();
        emitIfFalse(out, lhs, falseLbl);
        Operand rhs = irExpr(expr->children[1], out);
        emitIfFalse(out, rhs, falseLbl);
        emit(out, IR_ASSIGN, result, mkConstInt(1), noOperand());
        emitGoto(out, endLbl);
        emitLabel(out, falseLbl);
        emit(out, IR_ASSIGN, result, mkConstInt(0), noOperand());
    } else { /* || */
        Operand checkRhsLbl = mkLabel();
        Operand falseLbl = mkLabel();
        emitIfFalse(out, lhs, checkRhsLbl);
        emit(out, IR_ASSIGN, result, mkConstInt(1), noOperand());
        emitGoto(out, endLbl);
        emitLabel(out, checkRhsLbl);
        Operand rhs = irExpr(expr->children[1], out);
        emitIfFalse(out, rhs, falseLbl);
        emit(out, IR_ASSIGN, result, mkConstInt(1), noOperand());
        emitGoto(out, endLbl);
        emitLabel(out, falseLbl);
        emit(out, IR_ASSIGN, result, mkConstInt(0), noOperand());
    }
    emitLabel(out, endLbl);
    return result;
}

static Operand irAssign(ASTNode *expr, IRFunction *out) {
    ASTNode *lvalue = expr->children[0];
    Operand rhs = irExpr(expr->children[1], out);

    if (lvalue->kind == ND_ID) {
        Operand dst = mkVar(lvalue);
        emit(out, IR_ASSIGN, dst, rhs, noOperand());
        return dst;
    }
    /* ND_ARRAY_ACCESS: arr[idx] = rhs */
    Operand idx = irExpr(lvalue->children[0], out);
    Operand base = mkVar(lvalue);
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
            /* scalare */
            Operand init = irExpr(stmt->children[0], out);
            emit(out, IR_ASSIGN, mkVar(stmt), init, noOperand());
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
        Operand cond = irExpr(stmt->children[0], out);
        Operand elseLbl = mkLabel();
        emitIfFalse(out, cond, elseLbl);
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
        Operand cond = irExpr(stmt->children[0], out);
        emitIfFalse(out, cond, endLbl);
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

static IRFunction *irFunction(ASTNode *decl) {
    /* decl->text = "tipo nome" (vedi symtab_parse_decl_text); qui ci
       interessa solo il nome, in coda dopo l'ultimo spazio - stessa
       convenzione gia' usata dal resto del progetto per i decl-text. */
    const char *space = strrchr(decl->text, ' ');
    const char *name = space ? space + 1 : decl->text;

    IRFunction *f = calloc(1, sizeof(IRFunction));
    f->name = strdup(name);

    ASTNode *body = decl->children[decl->nchildren - 1];   /* ND_BLOCK, ultimo figlio */
    irStmt(body, f);
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
        free(prog->functions[i]);
    }
    free(prog->functions);
    free(prog);
}