/**
 * @file ir.c
 * @brief IR generation from the AST and IR-level optimisation pipeline.
 *
 * Cambiamenti rispetto alla versione precedente:
 *   - mkVar: scopeLevel==0 -> OPND_GLOBAL (espanso da ir_lower_globals)
 *   - ir_is_pure / ir_defines_dst: include IR_GLOBAL_ADDR
 *   - ir_buildFunction: chiama ir_lower_globals() dopo ir_resolveCFG()
 *     e prima di SVN/DCE/CP/LICM/SR
 *   - stampa debug: OPND_GLOBAL e IR_GLOBAL_ADDR
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "ir.h"
#include "global_lower.h"
#include "svn.h"
#include "dce.h"
#include "cp.h"
#include "licm.h"
#include "sr.h"
#include "sched.h"
#include "arena.h"

/* =========================================================================
 * Operator key helpers
 * ========================================================================= */
#define KEY_AND 0x2626
#define KEY_OR  0x7C7C
#define KEY_NOT 0x2100

static inline unsigned short op_key(const char *s) {
    if (!s || !s[0]) return 0;
    unsigned short k = (unsigned char)s[0] << 8;
    if (s[1]) k |= (unsigned char)s[1];
    return k;
}

/* =========================================================================
 * Module-level mutable state
 * ========================================================================= */

static int nextTemp;
static int nextLabel;
static int currentLoopDepth;

/* =========================================================================
 * Operand constructors
 * ========================================================================= */

static inline Operand mkTemp(void) {
    return (Operand){ .kind = OPND_TEMP, .data.tempId = nextTemp++ };
}

static inline Operand mkLabel(void) {
    return (Operand){ .kind = OPND_LABEL, .data.labelId = nextLabel++ };
}

/**
 * mkVar: distingue variabile locale (scopeLevel > 0) da globale (scopeLevel == 0).
 * Le globali vengono emesse come OPND_GLOBAL e saranno espanse da ir_lower_globals()
 * in IR_GLOBAL_ADDR + LOAD_ARR/STORE_ARR prima di ogni ottimizzazione.
 */
static inline Operand mkVar(const ASTNode *node) {
    if (node->scopeLevel == 0) {
        return (Operand){ .kind              = OPND_GLOBAL,
                          .data.globalOffset = node->offset };
    }
    return (Operand){ .kind            = OPND_VAR,
                      .data.varLevel   = node->scopeLevel,
                      .data.varOffset  = node->offset,
                      .data.sourceName = node->text };
}

static inline Operand mkConstInt(int v) {
    return (Operand){ .kind = OPND_CONST_INT, .data.intVal = v };
}

static inline Operand mkConstFloat(float v) {
    return (Operand){ .kind = OPND_CONST_FLOAT, .data.floatVal = v };
}

static inline Operand mkFunc(const char *name) {
    return (Operand){ .kind = OPND_FUNC, .data.funcName = name };
}

Operand noOperand(void) {
    return (Operand){ .kind = OPND_NONE };
}

/* =========================================================================
 * CFG helpers
 * ========================================================================= */

static inline int ir_isTerminator(IROp op) {
    const unsigned int mask =
        (1U << IR_GOTO) | (1U << IR_IF_FALSE) | (1U << IR_RETURN);
    return (mask & (1U << op)) != 0;
}

/**
 * IR_GLOBAL_ADDR e' pura: produce un indirizzo, nessun side-effect.
 * LICM puo' issarla, DCE puo' eliminarla se il risultato e' morto.
 */
int ir_is_pure(IROp op) {
    static const unsigned int mask =
        (1U << IR_ADD)  | (1U << IR_SUB) | (1U << IR_MUL) |
        (1U << IR_DIV)  | (1U << IR_MOD) | (1U << IR_NEG) |
        (1U << IR_NOT)  | (1U << IR_LT)  | (1U << IR_LE)  |
        (1U << IR_GT)   | (1U << IR_GE)  | (1U << IR_EQ)  |
        (1U << IR_NE)   | (1U << IR_ASSIGN) | (1U << IR_GLOBAL_ADDR);
    return (op < 32) && ((mask >> op) & 1U);
}

int ir_sweep(IRFunction *f, char *eliminate, int nBlocks) {
    int nInstrs    = f->count;
    IRInstr *newInstrs = malloc((size_t)nInstrs * sizeof(IRInstr));
    int newCount   = 0;

    for (int b = 0; b < nBlocks; b++) {
        int oldStart = f->blocks[b].bb.start;
        int oldEnd   = f->blocks[b].bb.end;
        int newStart = newCount;

        for (int i = oldStart; i < oldEnd; i++) {
            if (!eliminate[i])
                newInstrs[newCount++] = f->instrs[i];
        }

        f->blocks[b].bb.start = newStart;
        f->blocks[b].bb.end   = newCount;
    }

    free(f->instrs);
    f->instrs        = newInstrs;
    f->count         = newCount;
    f->capacity      = newCount;
    f->curBlockStart = 0;

    return (newCount != nInstrs);
}

static inline void ir_closeBlock(IRFunction *f, int start, int end) {
    if (end <= start) return;
    if (f->blockCount == f->blockCap) {
        f->blockCap = f->blockCap ? f->blockCap * 2 : 16;
        f->blocks = realloc(f->blocks, (size_t)f->blockCap * sizeof(IRBlock));
    }
    f->blocks[f->blockCount++] = (IRBlock){
        .bb       = { .start = start, .end = end, .succ = {-1, -1} },
        .predCount = 0,
    };
}

static void ir_registerLabel(IRFunction *f, int labelId, int futureBlockIdx) {
    int idx = labelId - f->labelBase;
    if (idx >= f->labelToBlockCap) {
        int newCap = f->labelToBlockCap ? f->labelToBlockCap : 8;
        while (newCap <= idx) newCap <<= 1;
        int *newTable = realloc(f->labelToBlock, (size_t)newCap * sizeof(int));
        if (!newTable) { fprintf(stderr, "OOM in ir_registerLabel\n"); exit(1); }
        f->labelToBlock = newTable;
        memset(f->labelToBlock + f->labelToBlockCap, -1,
               (size_t)(newCap - f->labelToBlockCap) * sizeof(int));
        f->labelToBlockCap = newCap;
    }
    f->labelToBlock[idx] = futureBlockIdx;
}

static void ir_emitInstr(IRFunction *f, IROp op, Operand dst, Operand src1, Operand src2) {
    if (f->count == f->capacity) {
        f->capacity = f->capacity ? f->capacity * 2 : 16;
        f->instrs = realloc(f->instrs, (size_t)f->capacity * sizeof(IRInstr));
    }
    int idx = f->count;

    if (op == IR_LABEL && idx > f->curBlockStart) {
        ir_closeBlock(f, f->curBlockStart, idx);
        f->curBlockStart = idx;
    }

    f->instrs[idx] = (IRInstr){
        .op        = op,
        .dst       = dst,
        .src1      = src1,
        .src2      = src2,
        .loopDepth = currentLoopDepth,
    };
    f->count++;

    if (op == IR_LABEL)
        ir_registerLabel(f, dst.data.labelId, f->blockCount);

    if (ir_isTerminator(op)) {
        ir_closeBlock(f, f->curBlockStart, f->count);
        f->curBlockStart = f->count;
    }
}

static inline void ir_emitGoto(IRFunction *f, Operand label) {
    ir_emitInstr(f, IR_GOTO, label, noOperand(), noOperand());
}
static inline void ir_emitIfFalse(IRFunction *f, Operand cond, Operand label) {
    ir_emitInstr(f, IR_IF_FALSE, label, cond, noOperand());
}
static inline void ir_emitLabel(IRFunction *f, Operand label) {
    ir_emitInstr(f, IR_LABEL, label, noOperand(), noOperand());
}

/* =========================================================================
 * CFG resolution
 * ========================================================================= */

static void ir_resolveCFG(IRFunction *f) {
    if (f->curBlockStart < f->count) {
        ir_closeBlock(f, f->curBlockStart, f->count);
        f->curBlockStart = f->count;
    }

    for (int b = 0; b < f->blockCount; b++) {
        int last = f->blocks[b].bb.end - 1;
        IROp op  = f->instrs[last].op;

        if (op == IR_GOTO) {
            int lbl = f->instrs[last].dst.data.labelId - f->labelBase;
            f->blocks[b].bb.succ[0] =
                (lbl >= 0 && lbl < f->labelToBlockCap) ? f->labelToBlock[lbl] : -1;

        } else if (op == IR_IF_FALSE) {
            f->blocks[b].bb.succ[0] = (b + 1 < f->blockCount) ? b + 1 : -1;
            int lbl = f->instrs[last].dst.data.labelId - f->labelBase;
            f->blocks[b].bb.succ[1] =
                (lbl >= 0 && lbl < f->labelToBlockCap) ? f->labelToBlock[lbl] : -1;

        } else if (op != IR_RETURN) {
            f->blocks[b].bb.succ[0] = (b + 1 < f->blockCount) ? b + 1 : -1;
        }
    }

    for (int b = 0; b < f->blockCount; b++)
        for (int k = 0; k < 2; k++) {
            int s = f->blocks[b].bb.succ[k];
            if (s >= 0) f->blocks[s].predCount++;
        }

    free(f->labelToBlock);
    f->labelToBlock    = NULL;
    f->labelToBlockCap = 0;
}

/* =========================================================================
 * Expression code generation — forward declarations
 * ========================================================================= */

static Operand ir_emitExpr(ASTNode *expr, IRFunction *out);
static Operand ir_emitExprInto(ASTNode *expr, IRFunction *out, Operand dest);
static void    ir_emitJumpIfFalse(ASTNode *cond, IRFunction *out, Operand falseLbl);
static void    ir_emitJumpIfTrue (ASTNode *cond, IRFunction *out, Operand trueLbl);

/* =========================================================================
 * Binary operator -> IROp mapping
 * ========================================================================= */

static inline IROp ir_binopToIROp(const char *op) {
    if (!op || op[0] == '\0') return IR_ADD;
    unsigned short key =
        (unsigned short)(((unsigned char)op[0] << 8) |
                         (unsigned char)(op[1] ? op[1] : 0));
    switch (key) {
    case (('+')<<8)|0:   return IR_ADD;
    case (('-')<<8)|0:   return IR_SUB;
    case (('*')<<8)|0:   return IR_MUL;
    case (('/')<<8)|0:   return IR_DIV;
    case (('%')<<8)|0:   return IR_MOD;
    case (('<')<<8)|0:   return IR_LT;
    case (('>')<<8)|0:   return IR_GT;
    case (('<')<<8)|'=': return IR_LE;
    case (('>')<<8)|'=': return IR_GE;
    case (('=')<<8)|'=': return IR_EQ;
    case (('!')<<8)|'=': return IR_NE;
    default:             return IR_ADD;
    }
}

/* =========================================================================
 * Short-circuit Boolean code generation
 * ========================================================================= */

static void ir_emitJumpIfFalse(ASTNode *cond, IRFunction *out, Operand falseLbl) {
    if (cond->kind == ND_BINOP && op_key(cond->text) == KEY_AND) {
        ir_emitJumpIfFalse(cond->children[0], out, falseLbl);
        ir_emitJumpIfFalse(cond->children[1], out, falseLbl);
        return;
    }
    if (cond->kind == ND_BINOP && op_key(cond->text) == KEY_OR) {
        Operand skipLbl = mkLabel();
        ir_emitJumpIfTrue(cond->children[0], out, skipLbl);
        ir_emitJumpIfFalse(cond->children[1], out, falseLbl);
        ir_emitLabel(out, skipLbl);
        return;
    }
    if (cond->kind == ND_UNARY && op_key(cond->text) == KEY_NOT) {
        ir_emitJumpIfTrue(cond->children[0], out, falseLbl);
        return;
    }
    Operand v = ir_emitExpr(cond, out);
    ir_emitIfFalse(out, v, falseLbl);
}

static void ir_emitJumpIfTrue(ASTNode *cond, IRFunction *out, Operand trueLbl) {
    if (cond->kind == ND_BINOP && op_key(cond->text) == KEY_AND) {
        Operand skipLbl = mkLabel();
        ir_emitJumpIfFalse(cond->children[0], out, skipLbl);
        ir_emitJumpIfTrue(cond->children[1], out, trueLbl);
        ir_emitLabel(out, skipLbl);
        return;
    }
    if (cond->kind == ND_BINOP && op_key(cond->text) == KEY_OR) {
        ir_emitJumpIfTrue(cond->children[0], out, trueLbl);
        ir_emitJumpIfTrue(cond->children[1], out, trueLbl);
        return;
    }
    if (cond->kind == ND_UNARY && op_key(cond->text) == KEY_NOT) {
        ir_emitJumpIfFalse(cond->children[0], out, trueLbl);
        return;
    }
    Operand v       = ir_emitExpr(cond, out);
    Operand skipLbl = mkLabel();
    ir_emitIfFalse(out, v, skipLbl);
    ir_emitGoto(out, trueLbl);
    ir_emitLabel(out, skipLbl);
}

static Operand ir_emitShortCircuitInto(ASTNode *expr, IRFunction *out, Operand dest) {
    Operand endLbl   = mkLabel();
    Operand falseLbl = mkLabel();
    ir_emitJumpIfFalse(expr, out, falseLbl);
    ir_emitInstr(out, IR_ASSIGN, dest, mkConstInt(1), noOperand());
    ir_emitGoto(out, endLbl);
    ir_emitLabel(out, falseLbl);
    ir_emitInstr(out, IR_ASSIGN, dest, mkConstInt(0), noOperand());
    ir_emitLabel(out, endLbl);
    return dest;
}

static Operand ir_emitShortCircuit(ASTNode *expr, IRFunction *out) {
    return ir_emitShortCircuitInto(expr, out, mkTemp());
}

/* =========================================================================
 * Assignment and call helpers
 * ========================================================================= */

static Operand ir_emitAssign(ASTNode *expr, IRFunction *out) {
    ASTNode *lvalue = expr->children[0];
    if (lvalue->kind == ND_ID)
        return ir_emitExprInto(expr->children[1], out, mkVar(lvalue));

    Operand idx  = ir_emitExpr(lvalue->children[0], out);
    Operand base = mkVar(lvalue);
    Operand rhs  = ir_emitExpr(expr->children[1], out);
    ir_emitInstr(out, IR_STORE_ARR, base, idx, rhs);
    return rhs;
}

static Operand ir_emitCall(ASTNode *expr, IRFunction *out) {
    for (int i = 0; i < expr->nchildren; i++) {
        Operand arg = ir_emitExpr(expr->children[i], out);
        ir_emitInstr(out, IR_PARAM, noOperand(), arg, noOperand());
    }
    Operand result = mkTemp();
    ir_emitInstr(out, IR_CALL, result, mkFunc(expr->text), mkConstInt(expr->nchildren));
    return result;
}

/* =========================================================================
 * ir_emitExpr
 * ========================================================================= */

static Operand ir_emitExpr(ASTNode *expr, IRFunction *out) {
    switch (expr->kind) {
    case ND_NUM_INT:   return mkConstInt(atoi(expr->text));
    case ND_NUM_FLOAT: return mkConstFloat((float)atof(expr->text));
    case ND_ID:        return mkVar(expr);

    case ND_ARRAY_ACCESS: {
        Operand idx  = ir_emitExpr(expr->children[0], out);
        Operand base = mkVar(expr);
        Operand t    = mkTemp();
        ir_emitInstr(out, IR_LOAD_ARR, t, base, idx);
        return t;
    }

    case ND_UNARY: {
        Operand v = ir_emitExpr(expr->children[0], out);
        Operand t = mkTemp();
        ir_emitInstr(out, op_key(expr->text) == KEY_NOT ? IR_NOT : IR_NEG, t, v, noOperand());
        return t;
    }

    case ND_BINOP: {
        unsigned short key = op_key(expr->text);
        if (key == KEY_AND || key == KEY_OR)
            return ir_emitShortCircuit(expr, out);
        Operand lhs = ir_emitExpr(expr->children[0], out);
        Operand rhs = ir_emitExpr(expr->children[1], out);
        Operand t   = mkTemp();
        ir_emitInstr(out, ir_binopToIROp(expr->text), t, lhs, rhs);
        return t;
    }

    case ND_ASSIGN: return ir_emitAssign(expr, out);
    case ND_CALL:   return ir_emitCall(expr, out);
    default:        return noOperand();
    }
}

static Operand ir_emitExprInto(ASTNode *expr, IRFunction *out, Operand dest) {
    switch (expr->kind) {
    case ND_NUM_INT:
        ir_emitInstr(out, IR_ASSIGN, dest, mkConstInt(atoi(expr->text)), noOperand());
        return dest;
    case ND_NUM_FLOAT:
        ir_emitInstr(out, IR_ASSIGN, dest, mkConstFloat((float)atof(expr->text)), noOperand());
        return dest;
    case ND_ID:
        ir_emitInstr(out, IR_ASSIGN, dest, mkVar(expr), noOperand());
        return dest;

    case ND_ARRAY_ACCESS: {
        Operand idx  = ir_emitExpr(expr->children[0], out);
        Operand base = mkVar(expr);
        ir_emitInstr(out, IR_LOAD_ARR, dest, base, idx);
        return dest;
    }

    case ND_UNARY: {
        Operand v = ir_emitExpr(expr->children[0], out);
        ir_emitInstr(out, op_key(expr->text) == KEY_NOT ? IR_NOT : IR_NEG, dest, v, noOperand());
        return dest;
    }

    case ND_BINOP: {
        unsigned short key = op_key(expr->text);
        if (key == KEY_AND || key == KEY_OR)
            return ir_emitShortCircuitInto(expr, out, dest);
        Operand lhs = ir_emitExpr(expr->children[0], out);
        Operand rhs = ir_emitExpr(expr->children[1], out);
        ir_emitInstr(out, ir_binopToIROp(expr->text), dest, lhs, rhs);
        return dest;
    }

    case ND_CALL: {
        for (int i = 0; i < expr->nchildren; i++) {
            Operand arg = ir_emitExpr(expr->children[i], out);
            ir_emitInstr(out, IR_PARAM, noOperand(), arg, noOperand());
        }
        ir_emitInstr(out, IR_CALL, dest, mkFunc(expr->text), mkConstInt(expr->nchildren));
        return dest;
    }

    case ND_ASSIGN: {
        Operand inner = ir_emitAssign(expr, out);
        ir_emitInstr(out, IR_ASSIGN, dest, inner, noOperand());
        return dest;
    }

    default: return noOperand();
    }
}

/* =========================================================================
 * Statement code generation
 * ========================================================================= */

static void ir_emitStmt(ASTNode *stmt, IRFunction *out) {
    if (!stmt) return;
    switch (stmt->kind) {

    case ND_BLOCK:
        for (int i = 0; i < stmt->nchildren; i++) ir_emitStmt(stmt->children[i], out);
        break;

    case ND_VAR_DECL:
        if (stmt->nchildren == 0) break;
        if (stmt->nchildren == 1) {
            ir_emitExprInto(stmt->children[0], out, mkVar(stmt));
        } else {
            for (int i = 0; i < stmt->nchildren; i++) {
                Operand v = ir_emitExpr(stmt->children[i], out);
                ir_emitInstr(out, IR_STORE_ARR, mkVar(stmt), mkConstInt(i), v);
            }
        }
        break;

    case ND_EXPR_STMT:
        ir_emitExpr(stmt->children[0], out);
        break;

    case ND_IF: {
        Operand elseLbl = mkLabel();
        ir_emitJumpIfFalse(stmt->children[0], out, elseLbl);
        ir_emitStmt(stmt->children[1], out);
        if (stmt->nchildren > 2) {
            Operand endLbl = mkLabel();
            ir_emitGoto(out, endLbl);
            ir_emitLabel(out, elseLbl);
            ir_emitStmt(stmt->children[2], out);
            ir_emitLabel(out, endLbl);
        } else {
            ir_emitLabel(out, elseLbl);
        }
        break;
    }

    case ND_WHILE: {
        Operand startLbl = mkLabel();
        Operand endLbl   = mkLabel();
        ir_emitLabel(out, startLbl);
        ir_emitJumpIfFalse(stmt->children[0], out, endLbl);
        currentLoopDepth++;
        ir_emitStmt(stmt->children[1], out);
        currentLoopDepth--;
        ir_emitGoto(out, startLbl);
        ir_emitLabel(out, endLbl);
        break;
    }

    case ND_RETURN: {
        Operand v = ir_emitExpr(stmt->children[0], out);
        ir_emitInstr(out, IR_RETURN, noOperand(), v, noOperand());
        break;
    }

    default: break;
    }
}

/* =========================================================================
 * Function compilation
 * ========================================================================= */

static IRFunction *ir_buildFunction(ASTNode *decl) {
    const char *space = strrchr(decl->text, ' ');
    const char *name  = space ? space + 1 : decl->text;

    IRFunction *f = calloc(1, sizeof(IRFunction));
    f->name          = strdup(name);
    f->labelBase     = nextLabel;
    f->curBlockStart = 0;
    currentLoopDepth = 0;

    ASTNode *body = decl->children[decl->nchildren - 1];
    ir_emitStmt(body, f);

    ir_resolveCFG(f);

    /* Lowering: OPND_GLOBAL -> IR_GLOBAL_ADDR + LOAD/STORE_ARR uniformi.
     * Deve avvenire PRIMA di SVN/DCE/CP per permettere a tutti gli ottimizzatori
     * di vedere IR omogeneo e ragionare su globali come su qualsiasi altra var. */
    ir_lower_globals(f);

    svn_optimize(f);
    dce_optimize(f);

    int changed;
    do {
        changed  = cp_optimize(f);
        changed |= dce_optimize(f);
    } while (changed);

    changed  = licm_optimize(f);
    changed |= sr_optimize(f);

    if (changed) {
        do {
            changed  = cp_optimize(f);
            changed |= dce_optimize(f);
        } while (changed);
    }

    return f;
}

/* =========================================================================
 * IRProgram management
 * ========================================================================= */

static void ir_programAppend(IRProgram *prog, IRFunction *f) {
    if (prog->count == prog->capacity) {
        prog->capacity = prog->capacity ? prog->capacity * 2 : 8;
        prog->functions = realloc(prog->functions,
                                  (size_t)prog->capacity * sizeof(IRFunction *));
    }
    prog->functions[prog->count++] = f;
}

static void ir_add_global(IRProgram *prog, ASTNode *decl, int symOffset) {
    if (!decl || decl->kind != ND_VAR_DECL) return;

    char *buf    = strdup(decl->text);
    char *space  = strchr(buf, ' ');
    if (!space) { free(buf); return; }
    *space       = '\0';
    char *tyName = buf;
    char *rest   = space + 1;

    int   isArray = 0, arraySize = 0;
    char *name;
    char *bracket = strchr(rest, '[');
    if (bracket) {
        *bracket  = '\0';
        name      = rest;
        isArray   = 1;
        arraySize = atoi(bracket + 1);
    } else {
        name = rest;
    }

    DataType dt = T_VOID;
    if      (tyName[0] == 'i') dt = T_INT;
    else if (tyName[0] == 'f') dt = T_FLOAT;

    if (prog->globalCount == prog->globalCap) {
        prog->globalCap = prog->globalCap ? prog->globalCap * 2 : 8;
        prog->globals   = realloc(prog->globals,
                                  (size_t)prog->globalCap * sizeof(IRGlobalVar));
    }

    IRGlobalVar *gv  = &prog->globals[prog->globalCount++];
    gv->name         = strdup(name);
    gv->dataType     = dt;
    gv->isArray      = isArray;
    gv->arraySize    = arraySize;
    gv->symOffset    = symOffset;
    gv->initVals     = NULL;
    gv->initCount    = 0;

    if (decl->nchildren > 0) {
        int cnt      = decl->nchildren;
        gv->initVals = malloc((size_t)cnt * sizeof(long));
        gv->initCount = cnt;
        for (int j = 0; j < cnt; j++) {
            ASTNode *ch = decl->children[j];
            if (ch->kind == ND_NUM_INT) {
                gv->initVals[j] = atol(ch->text);
            } else if (ch->kind == ND_NUM_FLOAT) {
                float fv = (float)atof(ch->text);
                long  lv;
                memcpy(&lv, &fv, sizeof fv);
                gv->initVals[j] = lv;
            } else {
                gv->initVals[j] = 0;
            }
        }
    }

    free(buf);
}

/* =========================================================================
 * Public API — ir_generate
 * ========================================================================= */

IRProgram *ir_generate(ASTNode *program) {
    nextTemp  = 0;
    nextLabel = 0;

    IRProgram *prog = calloc(1, sizeof(IRProgram));

    int symOffset = 0;
    for (int i = 0; i < program->nchildren; i++) {
        ASTNode *decl = program->children[i];
        if (decl->kind == ND_VAR_DECL)
            ir_add_global(prog, decl, symOffset);
        if (decl->kind == ND_VAR_DECL || decl->kind == ND_FUNC_DECL)
            symOffset++;
    }

    for (int i = 0; i < program->nchildren; i++) {
        ASTNode *decl = program->children[i];
        if (decl->kind == ND_FUNC_DECL)
            ir_programAppend(prog, ir_buildFunction(decl));
    }

    return prog;
}

/* =========================================================================
 * Debug printing
 * ========================================================================= */

static void ir_printOperand(const Operand *o) {
    switch (o->kind) {
    case OPND_NONE:        break;
    case OPND_TEMP:        printf("t%d", o->data.tempId); break;
    case OPND_VAR:
        printf("v%d.%d", o->data.varLevel, o->data.varOffset);
        if (o->data.sourceName) printf("/*%s*/", o->data.sourceName);
        break;
    case OPND_GLOBAL:      printf("g%d", o->data.globalOffset); break;
    case OPND_CONST_INT:   printf("%d",  o->data.intVal);            break;
    case OPND_CONST_FLOAT: printf("%g",  (double)o->data.floatVal);  break;
    case OPND_LABEL:       printf("L%d", o->data.labelId);           break;
    case OPND_FUNC:        printf("%s",  o->data.funcName);          break;
    }
}

static const char *ir_opMnemonic(IROp op) {
    switch (op) {
    case IR_ADD: return "+";  case IR_SUB: return "-";
    case IR_MUL: return "*";  case IR_DIV: return "/"; case IR_MOD: return "%";
    case IR_LT:  return "<";  case IR_LE:  return "<=";
    case IR_GT:  return ">";  case IR_GE:  return ">=";
    case IR_EQ:  return "=="; case IR_NE:  return "!=";
    default:     return "?";
    }
}

static void ir_printInstr(const IRInstr *in) {
    switch (in->op) {
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_LT:  case IR_LE:  case IR_GT:  case IR_GE:
    case IR_EQ:  case IR_NE:
        printf("    "); ir_printOperand(&in->dst);
        printf(" = ");  ir_printOperand(&in->src1);
        printf(" %s ", ir_opMnemonic(in->op));
        ir_printOperand(&in->src2);
        break;
    case IR_NEG:
        printf("    "); ir_printOperand(&in->dst);
        printf(" = -"); ir_printOperand(&in->src1);
        break;
    case IR_NOT:
        printf("    "); ir_printOperand(&in->dst);
        printf(" = !"); ir_printOperand(&in->src1);
        break;
    case IR_ASSIGN:
        printf("    "); ir_printOperand(&in->dst);
        printf(" = ");  ir_printOperand(&in->src1);
        break;
    case IR_GLOBAL_ADDR:
        printf("    "); ir_printOperand(&in->dst);
        printf(" = &g%d", in->src1.data.globalOffset);
        break;
    case IR_LOAD_ARR:
        printf("    "); ir_printOperand(&in->dst);
        printf(" = ");  ir_printOperand(&in->src1);
        printf("[");    ir_printOperand(&in->src2);
        printf("]");
        break;
    case IR_STORE_ARR:
        printf("    "); ir_printOperand(&in->dst);
        printf("[");    ir_printOperand(&in->src1);
        printf("] = "); ir_printOperand(&in->src2);
        break;
    case IR_PARAM:
        printf("    param "); ir_printOperand(&in->src1);
        break;
    case IR_CALL:
        printf("    "); ir_printOperand(&in->dst);
        printf(" = call "); ir_printOperand(&in->src1);
        printf(", ");       ir_printOperand(&in->src2);
        break;
    case IR_RETURN:
        printf("    return "); ir_printOperand(&in->src1);
        break;
    case IR_GOTO:
        printf("    goto "); ir_printOperand(&in->dst);
        break;
    case IR_IF_FALSE:
        printf("    if_false "); ir_printOperand(&in->src1);
        printf(" goto ");        ir_printOperand(&in->dst);
        break;
    case IR_LABEL:
        ir_printOperand(&in->dst); printf(":");
        break;
    }
    printf("\n");
}

void ir_print(const IRProgram *prog) {
    if (prog->globalCount > 0) {
        printf("=== GLOBALI ===\n");
        for (int i = 0; i < prog->globalCount; i++) {
            const IRGlobalVar *g = &prog->globals[i];
            printf("  [offset=%d] %s %s%s",
                   g->symOffset,
                   g->dataType == T_INT ? "int" : "float",
                   g->name,
                   g->isArray ? "[]" : "");
            if (g->initCount > 0) {
                printf(" = {");
                for (int j = 0; j < g->initCount; j++) {
                    if (j) printf(", ");
                    printf("%ld", g->initVals[j]);
                }
                printf("}");
            }
            printf("\n");
        }
        printf("\n");
    }

    for (int i = 0; i < prog->count; i++) {
        IRFunction *f = prog->functions[i];
        printf("funzione %s:\n", f->name);
        for (int j = 0; j < f->count; j++) ir_printInstr(&f->instrs[j]);
        printf("\n");
    }
}

/* =========================================================================
 * Public API — ir_free
 * ========================================================================= */

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

    for (int i = 0; i < prog->globalCount; i++) {
        free(prog->globals[i].name);
        free(prog->globals[i].initVals);
    }
    free(prog->globals);

    free(prog);
}

/* =========================================================================
 * IR front-end predicates
 * ========================================================================= */

int ir_defines_dst(IROp op) {
    static const uint32_t DEFINES_DST_MASK =
        (1u << IR_ADD)         | (1u << IR_SUB)  | (1u << IR_MUL)  |
        (1u << IR_DIV)         | (1u << IR_MOD)  | (1u << IR_NEG)  |
        (1u << IR_NOT)         | (1u << IR_LT)   | (1u << IR_LE)   |
        (1u << IR_GT)          | (1u << IR_GE)   | (1u << IR_EQ)   |
        (1u << IR_NE)          | (1u << IR_ASSIGN)                  |
        (1u << IR_GLOBAL_ADDR) | (1u << IR_LOAD_ARR) | (1u << IR_CALL);

    return (op < 32) && ((DEFINES_DST_MASK >> op) & 1u);
}

int ir_isCommutative(IROp op) {
    static const unsigned int mask =
        (1U << IR_ADD) | (1U << IR_MUL) | (1U << IR_EQ) | (1U << IR_NE);
    return (mask >> op) & 1U;
}

int ir_operand_is_storage(OperandKind kind) {
    /* OPND_GLOBAL NON e' storage: non viene tracciato da VarMap/liveness.
     * Sopravvive solo come src1 di IR_GLOBAL_ADDR, dove e' non-storage
     * esattamente come OPND_FUNC in IR_CALL. */
    return kind == OPND_VAR || kind == OPND_TEMP;
}