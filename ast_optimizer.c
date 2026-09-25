#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <float.h>
#include "ast_optimizer.h"

#define NUM_BUF_SIZE   (FLOAT_BUF_SIZE > INT_BUF_SIZE ? FLOAT_BUF_SIZE : INT_BUF_SIZE)

static ASTNode *rewrite_expr(Arena *arena, ASTNode *expr);
static ASTNode *rewrite_stmt(Arena *arena, ASTNode *stmt);
static inline int is_int_literal(ASTNode *n)     { return n && n->kind == ND_NUM_INT; }
static inline int is_float_literal(ASTNode *n)   { return n && n->kind == ND_NUM_FLOAT; }
static inline int is_numeric_literal(ASTNode *n) { return is_int_literal(n) || is_float_literal(n); }

static inline long   literal_as_long(ASTNode *n)   { return atol(n->text); }
static inline double literal_as_double(ASTNode *n) { return atof(n->text); }
static inline int    literal_is_zero(ASTNode *n)   { return literal_as_double(n) == 0.0; }

static inline int literal_int_equals(ASTNode *n, long v) {
    return is_int_literal(n) && literal_as_long(n) == v;
}


/* =========================================================================
 * SIDE-EFFECT ANALYSIS
 * ========================================================================= */

/**
 * @brief Analyzes an AST expression subtree to detect potential side effects.
 *
 * @details Recursively checks whether evaluating an expression alters execution state
 *          or could cause runtime traps (e.g., function calls, variable assignments,
 *          array indexing, or division/modulo operations that might divide by zero).
 *          This safety check prevents the optimizer from illegally pruning or transforming
 *          expressions (e.g., optimizing `foo() * 0` into `0`, which would skip `foo()`).
 *
 * @param expr Pointer to the AST expression node to analyze.
 * @return 1 if the expression contains side effects; 0 if it is side-effect-free.
 */
static int has_side_effect(ASTNode *expr) {
    if (!expr) return 0;

    switch (expr->kind) {
        /* Function calls, assignments, and array accesses modify state or may trap */
        case ND_CALL:
        case ND_ASSIGN:
        case ND_ARRAY_ACCESS:
            return 1;

        /* Division and modulo operations can trigger runtime division-by-zero exceptions */
        case ND_BINOP:
            if (expr->text[0] == '/' || expr->text[0] == '%') return 1;
            break;

        default:
            break;
    }

    for (int i = 0; i < expr->nchildren; i++) {
        if (has_side_effect(expr->children[i])) return 1;
    }

    return 0;
}


/* =========================================================================
 * UNARY LITERAL FOLD (single operand — not the two-literal path CP owns)
 * ========================================================================= */

/**
 * @brief Fold a unary operator applied to a numeric literal (-3, !0, …).
 *
 * Binary literal folding (3+2, 1<2) is deliberately absent: CP does that.
 */
static ASTNode *fold_unary_literal(Arena *arena, const char *op, ASTNode *child) {
    char buf[NUM_BUF_SIZE];

    if (op[0] == '-' && op[1] == '\0') {
        if (is_int_literal(child)) {
            snprintf(buf, sizeof(buf), "%ld", -literal_as_long(child));
            return newNode(arena, ND_NUM_INT, buf);
        }
        snprintf(buf, sizeof(buf), "%g", -literal_as_double(child));
        return newNode(arena, ND_NUM_FLOAT, buf);
    }

    if (op[0] == '!' && op[1] == '\0') {
        snprintf(buf, sizeof(buf), "%d", literal_is_zero(child) ? 1 : 0);
        return newNode(arena, ND_NUM_INT, buf);
    }

    return NULL;
}


/* =========================================================================
 * ASSOCIATIVE CHAIN FLATTEN + REBALANCE (int + and * only)
 * ========================================================================= */

/**
 * @brief Collect the leaves of a left/right-associated chain of the same operator.
 */
static void flatten_chain(ASTNode *expr, unsigned short opKey,
                          ASTNode ***leaves, int *count, int *cap) {
    if (expr && expr->kind == ND_BINOP &&
        OP_KEY(expr->text[0], expr->text[1]) == opKey) {
        flatten_chain(expr->children[0], opKey, leaves, count, cap);
        flatten_chain(expr->children[1], opKey, leaves, count, cap);
        return;
    }

    if (*count == *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *leaves = realloc(*leaves, (size_t)(*cap) * sizeof(ASTNode *));
    }
    (*leaves)[(*count)++] = expr;
}

static ASTNode *make_assoc_binop(Arena *arena, char opChar, ASTNode *left, ASTNode *right) {
    char text[2] = { opChar, '\0' };
    ASTNode *n = newNode(arena, ND_BINOP, text);
    addChild(arena, n, left);
    addChild(arena, n, right);
    n->dataType = T_INT;
    return n;
}

/**
 * @brief Pair-fold two adjacent int literals while rebuilding a +/ * chain.
 * 
 * This is part of rebalancing, not the general two-literal folder: it only
 * fires for int + and * (the ops this pass reassociates). 3+2 as a lone
 * binop never reaches here with count==2 in a way that changes semantics
 * vs CP — both produce 5. Kept so a+1+2 can collapse the constant pair
 * while the tree is rebuilt.
 */
static ASTNode *fold_assoc_int_pair(Arena *arena, char opChar, ASTNode *l, ASTNode *r) {
    if (!is_int_literal(l) || !is_int_literal(r)) return NULL;
    long a = literal_as_long(l);
    long b = literal_as_long(r);
    long res;
    if      (opChar == '+') res = a + b;
    else if (opChar == '*') res = a * b;
    else return NULL;
    char buf[NUM_BUF_SIZE];
    snprintf(buf, sizeof(buf), "%ld", res);
    ASTNode *n = newNode(arena, ND_NUM_INT, buf);
    n->dataType = T_INT;
    return n;
}

/**
 * @brief Rebuild @p leaves as a balanced +/ * tree, pairing left-to-right.
 *
 * Adjacent int literals in a pair are combined (partial fold). Non-literal
 * pairs become a new ND_BINOP. Repeats until a single root remains.
 */
static ASTNode *build_balanced_tree(Arena *arena, ASTNode **leaves, int count, char opChar) {
    if (count <= 0) return NULL;

    while (count > 1) {
        int writeIdx = 0;
        for (int i = 0; i < count; i += 2) {
            if (i + 1 == count) {
                leaves[writeIdx++] = leaves[i];
                continue;
            }
            ASTNode *folded = fold_assoc_int_pair(arena, opChar, leaves[i], leaves[i + 1]);
            if (folded)
                leaves[writeIdx++] = folded;
            else
                leaves[writeIdx++] = make_assoc_binop(arena, opChar, leaves[i], leaves[i + 1]);
        }
        count = writeIdx;
    }
    return leaves[0];
}

/**
 * @brief Coordinates the rebalancing of associative addition (+) and multiplication (*) chains.
 *
 * @param arena Pointer to the memory arena for node allocation.
 * @param expr  Pointer to the root node of the binary operation expression to balance.
 * @return Pointer to the rebalanced (and possibly partially folded) AST expression root.
 */
static ASTNode *balance_assoc_chain(Arena *arena, ASTNode *expr) {
    if (expr->text[0] != '+' && expr->text[0] != '*') return expr;
    if (expr->text[1] != '\0') return expr;
    /* float '+'/'*' is not associative (IEEE 754); dataType is stamped by semantic analysis */
    if (expr->dataType == T_FLOAT) return expr;

    char opChar = expr->text[0];
    unsigned short opKey = OP_KEY(opChar, 0);

    ASTNode **leaves = NULL;
    int count = 0, cap = 0;

    flatten_chain(expr, opKey, &leaves, &count, &cap);
    ASTNode *result = build_balanced_tree(arena, leaves, count, opChar);

    free(leaves);
    return result;
}


/* =========================================================================
 * ALGEBRAIC IDENTITIES (kept — not literal-literal arithmetic)
 * ========================================================================= */

/**
 * @brief Simplifies binary expressions based on algebraic identity laws.
 *
 * @param resultType  Type of the binary expression (from semantic stamp).
 *                    Float results skip identities that break IEEE 754
 *                    (x*0 must stay NaN/Inf; x+0 must preserve -0.0).
 * @return Pointer to simplified node, or `NULL` if no identity applies.
 */
static ASTNode *simplify_algebraic_identity(Arena *arena, unsigned short key,
                                            ASTNode *sx, ASTNode *dx,
                                            DataType resultType) {
    int isFloat = (resultType == T_FLOAT);

    switch (key) {
        case OP_KEY('+', 0):
            /* int only: -0.0 + 0.0 is +0.0 in IEEE; keeping sx would preserve -0.0 */
            if (!isFloat && literal_int_equals(dx, 0)) return sx; /* x + 0 -> x */
            if (!isFloat && literal_int_equals(sx, 0)) return dx; /* 0 + x -> x */
            break;

        case OP_KEY('-', 0):
            if (!isFloat && literal_int_equals(dx, 0)) return sx; /* x - 0 -> x */
            break;

        case OP_KEY('*', 0):
            if (literal_int_equals(dx, 1)) return sx; /* x * 1 -> x */
            if (literal_int_equals(sx, 1)) return dx; /* 1 * x -> x */

            /* int only: NaN*0 and Inf*0 must stay NaN/NaN under IEEE 754 */
            if (!isFloat && literal_int_equals(dx, 0) && !has_side_effect(sx)) {
                return newNode(arena, ND_NUM_INT, "0");
            }
            if (!isFloat && literal_int_equals(sx, 0) && !has_side_effect(dx)) {
                return newNode(arena, ND_NUM_INT, "0");
            }
            break;

        case OP_KEY('&','&'):
            if (literal_int_equals(sx, 0)) return newNode(arena, ND_NUM_INT, "0"); /* 0 && x -> 0 */
            if (literal_int_equals(sx, 1)) {
                /* 1 && x -> (x != 0): C requires boolean 0/1, not x itself */
                ASTNode *zero = newNode(arena, ND_NUM_INT, "0");
                ASTNode *ne = newNode(arena, ND_BINOP, "!=");
                addChild(arena, ne, dx);
                addChild(arena, ne, zero);
                return ne;
            }
            break;

        case OP_KEY('|','|'):
            if (literal_int_equals(sx, 1)) return newNode(arena, ND_NUM_INT, "1"); /* 1 || x -> 1 */
            if (literal_int_equals(sx, 0)) {
                /* 0 || x -> (x != 0) */
                ASTNode *zero = newNode(arena, ND_NUM_INT, "0");
                ASTNode *ne = newNode(arena, ND_BINOP, "!=");
                addChild(arena, ne, dx);
                addChild(arena, ne, zero);
                return ne;
            }
            break;

        default:
            break;
    }
    return NULL;
}


/* =========================================================================
 * EXPRESSION / STATEMENT REWRITE
 * ========================================================================= */

/**
 * @brief Handles rewrite and optimizations for unary expression nodes.
 */
static ASTNode *rewrite_unary_expr(Arena *arena, ASTNode *expr) {
    expr->children[0] = rewrite_expr(arena, expr->children[0]);
    ASTNode *child = expr->children[0];

    if (is_numeric_literal(child)) {
        ASTNode *folded = fold_unary_literal(arena, expr->text, child);
        if (folded) return folded;
    }
    return expr;
}

/** True if @p n is a '+' or '*' binary node (the ops handled by the balancer). */
static inline int is_assoc_binop(const ASTNode *n) {
    return n && n->kind == ND_BINOP && n->text[1] == '\0' &&
           (n->text[0] == '+' || n->text[0] == '*');
}

static ASTNode *rewrite_binop_impl(Arena *arena, ASTNode *expr, int deferBalance);

/* Rewrites one operand; a same-op child is a link of the parent's chain,
 * so it skips balancing (the chain root will rebalance the whole chain once). */
static ASTNode *rewrite_binop_operand(Arena *arena, ASTNode *child, const ASTNode *parent) {
    if (is_assoc_binop(parent) && child && child->kind == ND_BINOP &&
        child->text[0] == parent->text[0] && child->text[1] == '\0')
        return rewrite_binop_impl(arena, child, 1);
    return rewrite_expr(arena, child);
}

static ASTNode *rewrite_binop_impl(Arena *arena, ASTNode *expr, int deferBalance) {
    expr->children[0] = rewrite_binop_operand(arena, expr->children[0], expr);
    expr->children[1] = rewrite_binop_operand(arena, expr->children[1], expr);
    ASTNode *sx = expr->children[0];
    ASTNode *dx = expr->children[1];

    /*
     * Two-literal arithmetic / comparison / logic is NOT folded here.
     * CP on the IR is the single engine for 3+2, 1<2, 4/2, 5%2, 1&&0, …
     * Identities (x+0, x*1, 0&&y) still fire, including when one side is a literal.
     */
    unsigned short key = OP_KEY(expr->text[0], expr->text[1]);
    ASTNode *result = simplify_algebraic_identity(arena, key, sx, dx, expr->dataType);
    if (!result) result = expr;

    if (!deferBalance && is_assoc_binop(result))
        result = balance_assoc_chain(arena, result);

    return result;
}

/**
 * @brief True if @p expr is a unary +/-/! node (operator in text, one child).
 *
 * Avoids depending on the exact enum spelling (ND_UNARY / ND_UNOP): those
 * nodes are the only 1-child expressions whose text is a single operator.
 */
static int is_unary_op_node(const ASTNode *expr) {
    if (!expr || expr->kind == ND_BINOP || expr->nchildren != 1 || !expr->text)
        return 0;
    char c = expr->text[0];
    return (c == '-' || c == '!' || c == '+') && expr->text[1] == '\0';
}

static ASTNode *rewrite_expr(Arena *arena, ASTNode *expr) {
    if (!expr) return NULL;

    if (expr->kind == ND_BINOP)
        return rewrite_binop_impl(arena, expr, 0);

    if (is_unary_op_node(expr))
        return rewrite_unary_expr(arena, expr);

    for (int i = 0; i < expr->nchildren; i++)
        expr->children[i] = rewrite_expr(arena, expr->children[i]);

    return expr;
}

static ASTNode *rewrite_stmt(Arena *arena, ASTNode *stmt) {
    if (!stmt) return NULL;

    /* Function: rewrite the body only (last child). Params stay as declarations. */
    if (stmt->kind == ND_FUNC_DECL && stmt->nchildren > 0) {
        int last = stmt->nchildren - 1;
        stmt->children[last] = rewrite_stmt(arena, stmt->children[last]);
        return stmt;
    }

    return rewrite_expr(arena, stmt);
}

void optimize_ast(ASTNode *root, Arena *arena) {
    if (!root) return;
    for (int i = 0; i < root->nchildren; i++)
        root->children[i] = rewrite_stmt(arena, root->children[i]);
}