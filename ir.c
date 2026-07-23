#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ir.h"
#include "svn.h"

static int nextTemp;
static int nextLabel;

static Operand mkTemp(void) {
    Operand o; o.kind = OPND_TEMP; o.data.tempId = nextTemp++; return o;
}
static Operand mkLabel(void) {
    Operand o; o.kind = OPND_LABEL; o.data.labelId = nextLabel++; return o;
}
static Operand mkVar(const ASTNode *node) {
    Operand o;
    o.kind = OPND_VAR;
    o.data.varLevel = node->scopeLevel;
    o.data.varOffset = node->offset;
    o.data.sourceName = node->text;
    return o;
}
static Operand mkConstInt(int v) {
    Operand o; o.kind = OPND_CONST_INT; o.data.intVal = v; return o;
}
static Operand mkConstFloat(float v) {
    Operand o; o.kind = OPND_CONST_FLOAT; o.data.floatVal = v; return o;
}
static Operand mkFunc(const char *name) {
    Operand o; o.kind = OPND_FUNC; o.data.funcName = name; return o;
}
static Operand noOperand(void) {
    Operand o; o.kind = OPND_NONE; return o;
}

/* ---- Costruzione live del CFG ---- */
static int isTerminator(IROp op) {
    return op == IR_GOTO || op == IR_IF_FALSE || op == IR_RETURN;
}

static void closeBlock(IRFunction *f, int start, int end) {
    if (end <= start) return;
    if (f->blockCount == f->blockCap) {
        f->blockCap = f->blockCap ? f->blockCap * 2 : 16;
        f->blocks = realloc(f->blocks, (size_t) f->blockCap * sizeof(IRBlock));
    }
    IRBlock *b = &f->blocks[f->blockCount++];
    b->start = start;
    b->end = end;
    b->succ[0] = b->succ[1] = -1;
    b->predCount = 0;
}

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
        registerLabel(f, dst.data.labelId, f->blockCount);
    }
    if (isTerminator(op)) {
        closeBlock(f, f->curBlockStart, f->count);
        f->curBlockStart = f->count;
    }
}

static void emitGoto(IRFunction *f, Operand label) {
    emit(f, IR_GOTO, label, noOperand(), noOperand());
}
static void emitIfFalse(IRFunction *f, Operand cond, Operand label) {
    emit(f, IR_IF_FALSE, label, cond, noOperand());
}
static void emitLabel(IRFunction *f, Operand label) {
    emit(f, IR_LABEL, label, noOperand(), noOperand());
}

/* ---- Risoluzione del CFG ---- */
static void resolveCFG(IRFunction *f) {
    if (f->curBlockStart < f->count) {
        closeBlock(f, f->curBlockStart, f->count);
        f->curBlockStart = f->count;
    }

    for (int b = 0; b < f->blockCount; b++) {
        int last = f->blocks[b].end - 1;
        IROp op = f->instrs[last].op;
        if (op == IR_GOTO) {
            int lbl = f->instrs[last].dst.data.labelId - f->labelBase;
            f->blocks[b].succ[0] = (lbl >= 0 && lbl < f->labelToBlockCap) ? f->labelToBlock[lbl] : -1;
        } else if (op == IR_IF_FALSE) {
            f->blocks[b].succ[0] = (b + 1 < f->blockCount) ? b + 1 : -1;
            int lbl = f->instrs[last].dst.data.labelId - f->labelBase;
            f->blocks[b].succ[1] = (lbl >= 0 && lbl < f->labelToBlockCap) ? f->labelToBlock[lbl] : -1;
        } else if (op != IR_RETURN) {
            f->blocks[b].succ[0] = (b + 1 < f->blockCount) ? b + 1 : -1;
        }
    }

    for (int b = 0; b < f->blockCount; b++) {
        for (int k = 0; k < 2; k++) {
            int s = f->blocks[b].succ[k];
            if (s >= 0) f->blocks[s].predCount++;
        }
    }

    free(f->labelToBlock);
    f->labelToBlock = NULL;
    f->labelToBlockCap = 0;
}

/* ---- Traduzione ---- */
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
    return IR_ADD;
}

static Operand irExpr(ASTNode *expr, IRFunction *out);
static Operand irExprInto(ASTNode *expr, IRFunction *out, Operand dest);
static void irJumpIfFalse(ASTNode *cond, IRFunction *out, Operand falseLbl);
static void irJumpIfTrue(ASTNode *cond, IRFunction *out, Operand trueLbl);

static void irJumpIfFalse(ASTNode *cond, IRFunction *out, Operand falseLbl) {
    if (cond->kind == ND_BINOP && strcmp(cond->text, "&&") == 0) {
        irJumpIfFalse(cond->children[0], out, falseLbl);
        irJumpIfFalse(cond->children[1], out, falseLbl);
        return;
    }
    if (cond->kind == ND_BINOP && strcmp(cond->text, "||") == 0) {
        Operand skipLbl = mkLabel();
        irJumpIfTrue(cond->children[0], out, skipLbl);
        irJumpIfFalse(cond->children[1], out, falseLbl);
        emitLabel(out, skipLbl);
        return;
    }
    if (cond->kind == ND_UNARY && strcmp(cond->text, "!") == 0) {
        irJumpIfTrue(cond->children[0], out, falseLbl);
        return;
    }
    Operand v = irExpr(cond, out);
    emitIfFalse(out, v, falseLbl);
}

static void irJumpIfTrue(ASTNode *cond, IRFunction *out, Operand trueLbl) {
    if (cond->kind == ND_BINOP && strcmp(cond->text, "&&") == 0) {
        Operand skipLbl = mkLabel();
        irJumpIfFalse(cond->children[0], out, skipLbl);
        irJumpIfTrue(cond->children[1], out, trueLbl);
        emitLabel(out, skipLbl);
        return;
    }
    if (cond->kind == ND_BINOP && strcmp(cond->text, "||") == 0) {
        irJumpIfTrue(cond->children[0], out, trueLbl);
        irJumpIfTrue(cond->children[1], out, trueLbl);
        return;
    }
    if (cond->kind == ND_UNARY && strcmp(cond->text, "!") == 0) {
        irJumpIfFalse(cond->children[0], out, trueLbl);
        return;
    }
    Operand v = irExpr(cond, out);
    Operand skipLbl = mkLabel();
    emitIfFalse(out, v, skipLbl);
    emitGoto(out, trueLbl);
    emitLabel(out, skipLbl);
}

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
        return irExprInto(expr->children[1], out, mkVar(lvalue));
    }
    Operand idx = irExpr(lvalue->children[0], out);
    Operand base = mkVar(lvalue);
    Operand rhs = irExpr(expr->children[1], out);
    emit(out, IR_STORE_ARR, base, idx, rhs);
    return rhs;
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
    case ND_NUM_INT:    return mkConstInt(atoi(expr->text));
    case ND_NUM_FLOAT:  return mkConstFloat((float)atof(expr->text));
    case ND_ID:         return mkVar(expr);
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
        if (strcmp(expr->text, "&&") == 0 || strcmp(expr->text, "||") == 0)
            return irShortCircuit(expr, out);
        else {
            Operand lhs = irExpr(expr->children[0], out);
            Operand rhs = irExpr(expr->children[1], out);
            Operand t = mkTemp();
            emit(out, binopToIROp(expr->text), t, lhs, rhs);
            return t;
        }
    case ND_ASSIGN:     return irAssign(expr, out);
    case ND_CALL:       return irCall(expr, out);
    default:            return noOperand();
    }
}

static Operand irExprInto(ASTNode *expr, IRFunction *out, Operand dest) {
    switch (expr->kind) {
    case ND_NUM_INT:    emit(out, IR_ASSIGN, dest, mkConstInt(atoi(expr->text)), noOperand()); return dest;
    case ND_NUM_FLOAT:  emit(out, IR_ASSIGN, dest, mkConstFloat((float)atof(expr->text)), noOperand()); return dest;
    case ND_ID:         emit(out, IR_ASSIGN, dest, mkVar(expr), noOperand()); return dest;
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
        if (strcmp(expr->text, "&&") == 0 || strcmp(expr->text, "||") == 0)
            return irShortCircuitInto(expr, out, dest);
        else {
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
        Operand inner = irAssign(expr, out);
        emit(out, IR_ASSIGN, dest, inner, noOperand());
        return dest;
    }
    default: return noOperand();
    }
}

static void irStmt(ASTNode *stmt, IRFunction *out) {
    if (!stmt) return;
    switch (stmt->kind) {
    case ND_BLOCK:
        for (int i = 0; i < stmt->nchildren; i++) irStmt(stmt->children[i], out);
        break;
    case ND_VAR_DECL:
        if (stmt->nchildren == 0) break;
        if (stmt->nchildren == 1)
            irExprInto(stmt->children[0], out, mkVar(stmt));
        else
            for (int i = 0; i < stmt->nchildren; i++) {
                Operand v = irExpr(stmt->children[i], out);
                emit(out, IR_STORE_ARR, mkVar(stmt), mkConstInt(i), v);
            }
        break;
    case ND_EXPR_STMT:
        irExpr(stmt->children[0], out);
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
        Operand v = irExpr(stmt->children[0], out);
        emit(out, IR_RETURN, noOperand(), v, noOperand());
        break;
    }
    default: break;
    }
}

static IRFunction *irFunction(ASTNode *decl) {
    const char *space = strrchr(decl->text, ' ');
    const char *name = space ? space + 1 : decl->text;

    IRFunction *f = calloc(1, sizeof(IRFunction));
    f->name = strdup(name);
    f->labelBase = nextLabel;
    f->curBlockStart = 0;

    ASTNode *body = decl->children[decl->nchildren - 1];
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
        if (decl->kind != ND_FUNC_DECL) continue;
        programPush(prog, irFunction(decl));
    }
    return prog;
}

/* ---- Stampa ---- */
static void printOperand(const Operand *o) {
    switch (o->kind) {
    case OPND_NONE:        break;
    case OPND_TEMP:        printf("t%d", o->data.tempId); break;
    case OPND_VAR:
        printf("v%d.%d", o->data.varLevel, o->data.varOffset);
        if (o->data.sourceName) printf("/*%s*/", o->data.sourceName);
        break;
    case OPND_CONST_INT:   printf("%d", o->data.intVal); break;
    case OPND_CONST_FLOAT: printf("%g", (double)o->data.floatVal); break;
    case OPND_LABEL:       printf("L%d", o->data.labelId); break;
    case OPND_FUNC:        printf("%s", o->data.funcName); break;
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
    case IR_NEG: printf("    "); printOperand(&in->dst); printf(" = -"); printOperand(&in->src1); break;
    case IR_NOT: printf("    "); printOperand(&in->dst); printf(" = !"); printOperand(&in->src1); break;
    case IR_ASSIGN: printf("    "); printOperand(&in->dst); printf(" = "); printOperand(&in->src1); break;
    case IR_LOAD_ARR: printf("    "); printOperand(&in->dst); printf(" = "); printOperand(&in->src1);
                      printf("["); printOperand(&in->src2); printf("]"); break;
    case IR_STORE_ARR: printf("    "); printOperand(&in->dst); printf("["); printOperand(&in->src1);
                       printf("] = "); printOperand(&in->src2); break;
    case IR_PARAM: printf("    param "); printOperand(&in->src1); break;
    case IR_CALL: printf("    "); printOperand(&in->dst); printf(" = call ");
                  printOperand(&in->src1); printf(", "); printOperand(&in->src2); break;
    case IR_RETURN: printf("    return "); printOperand(&in->src1); break;
    case IR_GOTO: printf("    goto "); printOperand(&in->dst); break;
    case IR_IF_FALSE: printf("    if_false "); printOperand(&in->src1);
                      printf(" goto "); printOperand(&in->dst); break;
    case IR_LABEL: printOperand(&in->dst); printf(":"); break;
    }
    printf("\n");
}

void ir_print(const IRProgram *prog) {
    for (int i = 0; i < prog->count; i++) {
        IRFunction *f = prog->functions[i];
        printf("funzione %s:\n", f->name);
        for (int j = 0; j < f->count; j++) printInstr(&f->instrs[j]);
        printf("\n");
    }
}

void ir_free(IRProgram *prog) {
    if (!prog) return;
    for (int i = 0; i < prog->count; i++) {
        free(prog->functions[i]->name);
        free(prog->functions[i]->instrs);
        free(prog->functions[i]->blocks);
        free(prog->functions[i]->labelToBlock);
        free(prog->functions[i]);
    }
    free(prog->functions);
    free(prog);
}