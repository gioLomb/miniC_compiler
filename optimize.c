#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>   /* CHAR_BIT */
#include <float.h>    /* DECIMAL_DIG */
#include "optimize.h"

/*
 * Dimensione buffer per snprintf di valori numerici.
 *
 * INT_BUF_SIZE:   segno + cifre decimali di long + '\0'
 *                 Formula standard: CHAR_BIT*sizeof(long)/3 arrotonda
 *                 per eccesso il numero di cifre in base 10.
 *
 * FLOAT_BUF_SIZE: DECIMAL_DIG cifre significative (double) + overhead
 *                 per segno, punto decimale, 'e', segno esponente,
 *                 3 cifre esponente, '\0'.
 *
 * NUM_BUF_SIZE: massimo tra i due — usato nelle funzioni che possono
 *               produrre sia interi che float nello stesso buffer.
 */
#define INT_BUF_SIZE   (CHAR_BIT * sizeof(long) / 3 + 3)
#define FLOAT_BUF_SIZE (DECIMAL_DIG + 8)
#define NUM_BUF_SIZE   (FLOAT_BUF_SIZE > INT_BUF_SIZE ? FLOAT_BUF_SIZE : INT_BUF_SIZE)

/* =========================================================================
 * Helper di riconoscimento/lettura dei letterali
 * ========================================================================= */

static inline int isIntLiteral(ASTNode *n)     { return n && n->kind == ND_NUM_INT; }
static inline int isFloatLiteral(ASTNode *n)   { return n && n->kind == ND_NUM_FLOAT; }
static inline int isNumericLiteral(ASTNode *n) { return isIntLiteral(n) || isFloatLiteral(n); }

static inline long   literalAsLong(ASTNode *n)   { return atol(n->text); }
static inline double literalAsDouble(ASTNode *n) { return atof(n->text); }
static inline int    literalIsZero(ASTNode *n)   { return literalAsDouble(n) == 0.0; }

static inline int literalIntEquals(ASTNode *n, long v) {
    return isIntLiteral(n) && literalAsLong(n) == v;
}

static void freeNodeShallow(ASTNode *node) {
    if (!node) return;
    free(node->children);
    free(node->text);
    free(node);
}

/* =========================================================================
 * hasSideEffect
 * ========================================================================= */
static int hasSideEffect(ASTNode *expr) {
    if (!expr) return 0;

    switch (expr->kind) {
        case ND_CALL:
        case ND_ASSIGN:
        case ND_ARRAY_ACCESS:
            return 1;
        case ND_BINOP:
            if (expr->text[0] == '/' || expr->text[0] == '%')
                return 1;
            break;
        default:
            break;
    }

    for (int i = 0; i < expr->nchildren; i++) {
        if (hasSideEffect(expr->children[i])) return 1;
    }
    return 0;
}

/* =========================================================================
 * foldBinopLiterals
 * ========================================================================= */
static ASTNode *foldBinopLiterals(const char *op, ASTNode *sx, ASTNode *dx) {
    int bothInt = isIntLiteral(sx) && isIntLiteral(dx);

    char buf[NUM_BUF_SIZE];
    unsigned short key = OP_KEY(op[0], op[1]);

    switch (key) {
        case OP_KEY('=','='): case OP_KEY('!','='):
        case OP_KEY('<', 0):  case OP_KEY('>', 0):
        case OP_KEY('<','='): case OP_KEY('>','='):
        case OP_KEY('&','&'): case OP_KEY('|','|'): {
            double a = literalAsDouble(sx), b = literalAsDouble(dx);
            int result;
            switch (key) {
                case OP_KEY('=','='): result = (a == b); break;
                case OP_KEY('!','='): result = (a != b); break;
                case OP_KEY('<', 0):  result = (a <  b); break;
                case OP_KEY('>', 0):  result = (a >  b); break;
                case OP_KEY('<','='): result = (a <= b); break;
                case OP_KEY('>','='): result = (a >= b); break;
                case OP_KEY('&','&'): result = (a != 0.0 && b != 0.0); break;
                default:              result = (a != 0.0 || b != 0.0); break;
            }
            snprintf(buf, sizeof(buf), "%d", result);
            return newNode(ND_NUM_INT, buf);
        }
        default:
            break;
    }

    if (key == OP_KEY('%', 0)) {
        long b = literalAsLong(dx);
        if (b == 0) return NULL;
        long a = literalAsLong(sx);
        snprintf(buf, sizeof(buf), "%ld", a % b);
        return newNode(ND_NUM_INT, buf);
    }

    if (key == OP_KEY('/', 0)) {
        if (bothInt) {
            long b = literalAsLong(dx);
            if (b == 0) return NULL;
            long a = literalAsLong(sx);
            snprintf(buf, sizeof(buf), "%ld", a / b);
            return newNode(ND_NUM_INT, buf);
        }
        double b = literalAsDouble(dx);
        if (b == 0.0) return NULL;
        double a = literalAsDouble(sx);
        snprintf(buf, sizeof(buf), "%g", a / b);
        return newNode(ND_NUM_FLOAT, buf);
    }

    if (bothInt) {
        long a = literalAsLong(sx), b = literalAsLong(dx), r;
        switch (key) {
            case OP_KEY('+', 0): r = a + b; break;
            case OP_KEY('-', 0): r = a - b; break;
            default:             r = a * b; break;
        }
        snprintf(buf, sizeof(buf), "%ld", r);
        return newNode(ND_NUM_INT, buf);
    }
    {
        double a = literalAsDouble(sx), b = literalAsDouble(dx), r;
        switch (key) {
            case OP_KEY('+', 0): r = a + b; break;
            case OP_KEY('-', 0): r = a - b; break;
            default:             r = a * b; break;
        }
        snprintf(buf, sizeof(buf), "%g", r);
        return newNode(ND_NUM_FLOAT, buf);
    }
}

/* =========================================================================
 * foldUnaryLiteral
 * ========================================================================= */
static ASTNode *foldUnaryLiteral(const char *op, ASTNode *child) {
    char buf[NUM_BUF_SIZE];
    if (op[0] == '-') {
        if (isIntLiteral(child)) {
            snprintf(buf, sizeof(buf), "%ld", -literalAsLong(child));
            return newNode(ND_NUM_INT, buf);
        }
        snprintf(buf, sizeof(buf), "%g", -literalAsDouble(child));
        return newNode(ND_NUM_FLOAT, buf);
    }
    if (op[0] == '!') {
        snprintf(buf, sizeof(buf), "%d", literalIsZero(child) ? 1 : 0);
        return newNode(ND_NUM_INT, buf);
    }
    return NULL;
}

/* =========================================================================
 * Tree height balancing
 * ========================================================================= */

static int containsFloatLiteral(ASTNode *n) {
    if (!n) return 0;
    if (n->kind == ND_NUM_FLOAT) return 1;
    for (int i = 0; i < n->nchildren; i++) {
        if (containsFloatLiteral(n->children[i])) return 1;
    }
    return 0;
}

static void flattenChain(ASTNode *node, unsigned short opKey,
                          ASTNode ***leaves, int *count, int *cap) {
    if (node->kind == ND_BINOP &&
        OP_KEY(node->text[0], node->text[1]) == opKey) {
        flattenChain(node->children[0], opKey, leaves, count, cap);
        flattenChain(node->children[1], opKey, leaves, count, cap);
        freeNodeShallow(node);
    } else {
        if (*count == *cap) {
            *cap = *cap ? *cap * 2 : 4;
            *leaves = realloc(*leaves, (size_t)*cap * sizeof(ASTNode *));
        }
        (*leaves)[(*count)++] = node;
    }
}

static ASTNode *buildBalanced(ASTNode **leaves, int count, const char opChar) {
    while (count > 1) {
        int writeIdx = 0;
        int i = 0;
        for (; i + 1 < count; i += 2) {
            ASTNode *sx = leaves[i];
            ASTNode *dx = leaves[i + 1];

            if (isNumericLiteral(sx) && isNumericLiteral(dx)) {
                char opStr[3] = { opChar, '\0', '\0' };
                ASTNode *folded = foldBinopLiterals(opStr, sx, dx);
                if (folded) {
                    freeAST(sx);
                    freeAST(dx);
                    leaves[writeIdx++] = folded;
                    continue;
                }
            }

            ASTNode *pair = newNode(ND_BINOP, (char[]){ opChar, '\0' });
            addChild(pair, sx);
            addChild(pair, dx);
            leaves[writeIdx++] = pair;
        }
        if (i < count)
            leaves[writeIdx++] = leaves[i];
        count = writeIdx;
    }
    return leaves[0];
}

static ASTNode *balanceAssocChain(ASTNode *expr) {
    if (expr->text[0] != '+' && expr->text[0] != '*') return expr;
    if (expr->text[1] != '\0') return expr;
    if (containsFloatLiteral(expr)) return expr;

    char opChar = expr->text[0];
    unsigned short opKey = OP_KEY(opChar, 0);

    ASTNode **leaves = NULL;
    int count = 0, cap = 0;
    flattenChain(expr, opKey, &leaves, &count, &cap);

    ASTNode *result = buildBalanced(leaves, count, opChar);
    free(leaves);
    return result;
}

/* =========================================================================
 * Attraversamento espressioni
 * ========================================================================= */

static ASTNode *optimizeExpr(ASTNode *expr) {
    if (!expr) return NULL;

    switch (expr->kind) {

    case ND_NUM_INT:
    case ND_NUM_FLOAT:
    case ND_ID:
        return expr;

    case ND_UNARY: {
        expr->children[0] = optimizeExpr(expr->children[0]);
        ASTNode *child = expr->children[0];

        if (isNumericLiteral(child)) {
            ASTNode *folded = foldUnaryLiteral(expr->text, child);
            if (folded) {
                freeAST(expr);
                return folded;
            }
        }
        return expr;
    }

    case ND_BINOP: {
        expr->children[0] = optimizeExpr(expr->children[0]);
        expr->children[1] = optimizeExpr(expr->children[1]);
        ASTNode *sx = expr->children[0];
        ASTNode *dx = expr->children[1];

        if (isNumericLiteral(sx) && isNumericLiteral(dx)) {
            ASTNode *folded = foldBinopLiterals(expr->text, sx, dx);
            if (folded) {
                freeAST(expr);
                return folded;
            }
        }

        unsigned short key = OP_KEY(expr->text[0], expr->text[1]);
        switch (key) {
            case OP_KEY('+', 0):
                if (literalIntEquals(dx, 0)) { freeNodeShallow(dx); freeNodeShallow(expr); return sx; }
                if (literalIntEquals(sx, 0)) { freeNodeShallow(sx); freeNodeShallow(expr); return dx; }
                break;
            case OP_KEY('-', 0):
                if (literalIntEquals(dx, 0)) { freeNodeShallow(dx); freeNodeShallow(expr); return sx; }
                break;
            case OP_KEY('*', 0):
                if (literalIntEquals(dx, 1)) { freeNodeShallow(dx); freeNodeShallow(expr); return sx; }
                if (literalIntEquals(sx, 1)) { freeNodeShallow(sx); freeNodeShallow(expr); return dx; }
                if (literalIntEquals(dx, 0) && !hasSideEffect(sx)) { freeAST(expr); return newNode(ND_NUM_INT, "0"); }
                if (literalIntEquals(sx, 0) && !hasSideEffect(dx)) { freeAST(expr); return newNode(ND_NUM_INT, "0"); }
                break;
            case OP_KEY('&','&'):
                if (literalIntEquals(sx, 0)) { freeAST(expr); return newNode(ND_NUM_INT, "0"); }
                if (literalIntEquals(sx, 1)) { freeNodeShallow(sx); freeNodeShallow(expr); return dx; }
                break;
            case OP_KEY('|','|'):
                if (literalIntEquals(sx, 1)) { freeAST(expr); return newNode(ND_NUM_INT, "1"); }
                if (literalIntEquals(sx, 0)) { freeNodeShallow(sx); freeNodeShallow(expr); return dx; }
                break;
            default:
                break;
        }

        return balanceAssocChain(expr);
    }

    case ND_ASSIGN:
        expr->children[0] = optimizeExpr(expr->children[0]);
        expr->children[1] = optimizeExpr(expr->children[1]);
        return expr;

    case ND_CALL:
        for (int i = 0; i < expr->nchildren; i++)
            expr->children[i] = optimizeExpr(expr->children[i]);
        return expr;

    case ND_ARRAY_ACCESS:
        expr->children[0] = optimizeExpr(expr->children[0]);
        return expr;

    default:
        return expr;
    }
}

/* =========================================================================
 * Attraversamento statement
 * ========================================================================= */

static ASTNode *optimizeStmt(ASTNode *stmt) {
    if (!stmt) return NULL;

    switch (stmt->kind) {

    case ND_VAR_DECL:
        for (int i = 0; i < stmt->nchildren; i++)
            stmt->children[i] = optimizeExpr(stmt->children[i]);
        return stmt;

    case ND_BLOCK: {
        ASTNode **oldChildren = stmt->children;
        int oldCount = stmt->nchildren;

        stmt->children = (oldCount > 0)
                         ? malloc((size_t)oldCount * sizeof(ASTNode *))
                         : NULL;
        stmt->nchildren = 0;
        stmt->capacity  = oldCount;

        for (int i = 0; i < oldCount; i++) {
            ASTNode *result = optimizeStmt(oldChildren[i]);
            if (!result) continue;

            if (result->kind == ND_BLOCK) {
                for (int j = 0; j < result->nchildren; j++)
                    addChild(stmt, result->children[j]);
                freeNodeShallow(result);
            } else {
                addChild(stmt, result);
            }
        }

        free(oldChildren);
        return stmt;
    }

    case ND_IF: {
        stmt->children[0] = optimizeExpr(stmt->children[0]);
        ASTNode *cond = stmt->children[0];

        if (isNumericLiteral(cond)) {
            int condTrue = !literalIsZero(cond);
            ASTNode *thenBr = stmt->children[1];
            ASTNode *elseBr = (stmt->nchildren > 2) ? stmt->children[2] : NULL;

            ASTNode *survivor   = condTrue ? thenBr : elseBr;
            ASTNode *deadBranch = condTrue ? elseBr : thenBr;

            freeAST(cond);
            if (deadBranch) freeAST(deadBranch);
            freeNodeShallow(stmt);

            return survivor ? optimizeStmt(survivor) : NULL;
        }

        stmt->children[1] = optimizeStmt(stmt->children[1]);
        if (!stmt->children[1]) stmt->children[1] = newNode(ND_BLOCK, NULL);

        if (stmt->nchildren > 2) {
            stmt->children[2] = optimizeStmt(stmt->children[2]);
            if (!stmt->children[2]) stmt->children[2] = newNode(ND_BLOCK, NULL);
        }
        return stmt;
    }

    case ND_WHILE: {
        stmt->children[0] = optimizeExpr(stmt->children[0]);
        ASTNode *cond = stmt->children[0];

        if (isNumericLiteral(cond) && literalIsZero(cond)) {
            freeAST(stmt);
            return NULL;
        }

        stmt->children[1] = optimizeStmt(stmt->children[1]);
        if (!stmt->children[1]) stmt->children[1] = newNode(ND_BLOCK, NULL);
        return stmt;
    }

    case ND_RETURN:
    case ND_EXPR_STMT:
        stmt->children[0] = optimizeExpr(stmt->children[0]);
        return stmt;

    default:
        return stmt;
    }
}

/* =========================================================================
 * Entry point
 * ========================================================================= */

void optimize_ast(ASTNode *program) {
    if (!program) return;

    for (int i = 0; i < program->nchildren; i++) {
        ASTNode *decl = program->children[i];
        if (!decl) continue;

        if (decl->kind == ND_FUNC_DECL) {
            ASTNode *body = decl->children[decl->nchildren - 1];
            ASTNode *optimizedBody = optimizeStmt(body);
            if (!optimizedBody) optimizedBody = newNode(ND_BLOCK, NULL);
            decl->children[decl->nchildren - 1] = optimizedBody;

        } else if (decl->kind == ND_VAR_DECL) {
            for (int c = 0; c < decl->nchildren; c++)
                decl->children[c] = optimizeExpr(decl->children[c]);
        }
    }
}