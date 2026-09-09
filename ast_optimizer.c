#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <float.h>
#include "ast_optimizer.h"

// Calculate maximum buffer size required to safely convert numeric values into string representation
#define INT_BUF_SIZE   (CHAR_BIT * sizeof(long) / 3 + 3)
#define FLOAT_BUF_SIZE (DECIMAL_DIG + 8)
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


// SIDE-EFFECT ANALYSIS

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
        // Function calls, assignments, and array accesses modify state or may trap
        case ND_CALL:
        case ND_ASSIGN:
        case ND_ARRAY_ACCESS:
            return 1;

        // Division and modulo operations can trigger runtime division-by-zero exceptions
        case ND_BINOP:
            if (expr->text[0] == '/' || expr->text[0] == '%') return 1;
            break;

        default:
            break;
    }

    // Recursively check child expressions
    for (int i = 0; i < expr->nchildren; i++) {
        if (has_side_effect(expr->children[i])) return 1;
    }

    return 0;
}


/**
 * @brief Folds comparison and logical binary operations on literal values.
 */
static ASTNode *fold_comparison_and_logic(Arena *arena, unsigned short key,
                                          ASTNode *sx, ASTNode *dx) {
    double a = literal_as_double(sx);
    double b = literal_as_double(dx);
    int result;

    switch (key) {
        case OP_KEY('=','='): result = (a == b); break;
        case OP_KEY('!','='): result = (a != b); break;
        case OP_KEY('<', 0):  result = (a <  b); break;
        case OP_KEY('>', 0):  result = (a >  b); break;
        case OP_KEY('<','='): result = (a <= b); break;
        case OP_KEY('>','='): result = (a >= b); break;
        case OP_KEY('&','&'): result = (a != 0.0 && b != 0.0); break;
        case OP_KEY('|','|'): result = (a != 0.0 || b != 0.0); break;
        default: return NULL;
    }

    char buf[NUM_BUF_SIZE];
    snprintf(buf, sizeof(buf), "%d", result);
    return newNode(arena, ND_NUM_INT, buf);
}

/**
 * @brief Folds integer modulo operation safely at compile time.
 */
static ASTNode *fold_modulo(Arena *arena, ASTNode *sx, ASTNode *dx) {
    long b = literal_as_long(dx);
    if (b == 0) return NULL; // Abort folding to defer exception to runtime

    char buf[NUM_BUF_SIZE];
    snprintf(buf, sizeof(buf), "%ld", literal_as_long(sx) % b);
    return newNode(arena, ND_NUM_INT, buf);
}

/**
 * @brief Folds division operation safely for integer and floating-point literals.
 */
static ASTNode *fold_division(Arena *arena, ASTNode *sx, ASTNode *dx, int bothInt) {
    char buf[NUM_BUF_SIZE];

    if (bothInt) {
        long b = literal_as_long(dx);
        if (b == 0) return NULL; // Abort folding
        snprintf(buf, sizeof(buf), "%ld", literal_as_long(sx) / b);
        return newNode(arena, ND_NUM_INT, buf);
    }

    double b = literal_as_double(dx);
    if (b == 0.0) return NULL; // Abort folding
    snprintf(buf, sizeof(buf), "%g", literal_as_double(sx) / b);
    return newNode(arena, ND_NUM_FLOAT, buf);
}

/**
 * @brief Folds basic arithmetic operations (+, -, *) for numeric literals.
 */
static ASTNode *fold_arithmetic(Arena *arena, unsigned short key,
                                ASTNode *sx, ASTNode *dx, int bothInt) {
    char buf[NUM_BUF_SIZE];

    if (bothInt) {
        long a = literal_as_long(sx);
        long b = literal_as_long(dx);
        long r;
        switch (key) {
            case OP_KEY('+', 0): r = a + b; break;
            case OP_KEY('-', 0): r = a - b; break;
            default:             r = a * b; break;
        }
        snprintf(buf, sizeof(buf), "%ld", r);
        return newNode(arena, ND_NUM_INT, buf);
    }

    double a = literal_as_double(sx);
    double b = literal_as_double(dx);
    double r;
    switch (key) {
        case OP_KEY('+', 0): r = a + b; break;
        case OP_KEY('-', 0): r = a - b; break;
        default:             r = a * b; break;
    }
    snprintf(buf, sizeof(buf), "%g", r);
    return newNode(arena, ND_NUM_FLOAT, buf);
}

/**
 * @brief Evaluates a binary operation on two numeric literal AST nodes at compile time.
 *
 * @param arena Pointer to the memory arena used to allocate the new folded result node.
 * @param op    Null-terminated string representing the binary operator.
 * @param sx    Pointer to the left-hand operand AST node (must be numeric literal).
 * @param dx    Pointer to the right-hand operand AST node (must be numeric literal).
 * @return Pointer to a new folded AST node, or `NULL` if folding cannot be performed safely.
 */
static ASTNode *fold_binop_literals(Arena *arena, const char *op, ASTNode *sx, ASTNode *dx) {
    unsigned short key = OP_KEY(op[0], op[1]);

    // Comparison and logical operators
    ASTNode *foldedComp = fold_comparison_and_logic(arena, key, sx, dx);
    if (foldedComp) return foldedComp;

    // Modulo operator
    if (key == OP_KEY('%', 0)) {
        return fold_modulo(arena, sx, dx);
    }

    int bothInt = is_int_literal(sx) && is_int_literal(dx);

    // Division operator
    if (key == OP_KEY('/', 0)) {
        return fold_division(arena, sx, dx, bothInt);
    }

    // Basic arithmetic operations (+, -, *)
    return fold_arithmetic(arena, key, sx, dx, bothInt);
}

/**
 * @brief Folds a unary operation (negation or logical NOT) on a literal.
 *
 * @param arena Pointer to the memory arena used for node allocation.
 * @param op    Operator character string (`-` or `!`).
 * @param child Pointer to the literal child operand AST node.
 * @return Pointer to a new folded AST node, or `NULL` if not foldable.
 */
static ASTNode *fold_unary_literal(Arena *arena, const char *op, ASTNode *child) {
    char buf[NUM_BUF_SIZE];

    // Arithmetic negation (-)
    if (op[0] == '-') {
        if (is_int_literal(child)) {
            snprintf(buf, sizeof(buf), "%ld", -literal_as_long(child));
            return newNode(arena, ND_NUM_INT, buf);
        }
        snprintf(buf, sizeof(buf), "%g", -literal_as_double(child));
        return newNode(arena, ND_NUM_FLOAT, buf);
    }

    // Logical NOT (!)
    if (op[0] == '!') {
        snprintf(buf, sizeof(buf), "%d", literal_is_zero(child) ? 1 : 0);
        return newNode(arena, ND_NUM_INT, buf);
    }
    return NULL;
}

// 
// TREE HEIGHT BALANCING FOR ASSOCIATIVE OPERATORS
// 

/**
 * @brief Checks if a subtree contains floating-point literals.
 *
 * @details Floating-point reordering is disabled to preserve strict IEEE 754 precision semantics.
 *
 * @param n Pointer to the AST node to check.
 * @return 1 if floating-point literals exist in the subtree; 0 otherwise.
 */
static int contains_float_literal(ASTNode *n) {
    if (!n) return 0;
    if (n->kind == ND_NUM_FLOAT) return 1;

    for (int i = 0; i < n->nchildren; i++) {
        if (contains_float_literal(n->children[i])) return 1;
    }
    return 0;
}

/**
 * @brief Recursively flattens a chain of identical associative binary operations into a flat leaf array.
 *
 * @param node   Pointer to the current AST node in the associative chain.
 * @param opKey  16-bit key representing the target associative operator to flatten.
 * @param leaves Double pointer to a dynamically allocated array of leaf AST node pointers.
 * @param count  Pointer to the current count of collected leaf nodes in the array.
 * @param cap    Pointer to the current capacity of the `leaves` array.
 */
static void flatten_chain(ASTNode *node, unsigned short opKey,
                         ASTNode ***leaves, int *count, int *cap) {
    if (node->kind == ND_BINOP && OP_KEY(node->text[0], node->text[1]) == opKey) {
        flatten_chain(node->children[0], opKey, leaves, count, cap);
        flatten_chain(node->children[1], opKey, leaves, count, cap);
        // 'node' itself (the binary wrapper) is abandoned in the arena here
    } else {
        if (*count == *cap) {
            *cap = *cap ? *cap * 2 : 4;
            *leaves = realloc(*leaves, (size_t)*cap * sizeof(ASTNode *));
        }
        (*leaves)[(*count)++] = node;
    }
}

/**
 * @brief Rebuilds a balanced binary tree from an array of leaf nodes and performs instant folding.
 *
 * @param arena    Pointer to the memory arena used for allocating new binary nodes.
 * @param leaves   Array of pointers to AST leaf subtrees.
 * @param count    Number of valid leaf entries in the array.
 * @param opChar   Operator character (`+` or `*`) used for connecting the pairs.
 * @return Pointer to the root node of the freshly built, height-balanced AST subtree.
 */
static ASTNode *build_balanced_tree(Arena *arena, ASTNode **leaves, int count, char opChar) {
    while (count > 1) {
        int writeIdx = 0, i = 0;
        for (; i + 1 < count; i += 2) {
            ASTNode *sx = leaves[i];
            ASTNode *dx = leaves[i + 1];

            // Attempt immediate constant folding on adjacent literal pair
            if (is_numeric_literal(sx) && is_numeric_literal(dx)) {
                char opStr[3] = { opChar, '\0', '\0' };
                ASTNode *folded = fold_binop_literals(arena, opStr, sx, dx);
                if (folded) {
                    // sx/dx abandoned in the arena, replaced by the folded literal
                    leaves[writeIdx++] = folded;
                    continue;
                }
            }

            // Construct balanced binary node pair
            ASTNode *pair = newNode(arena, ND_BINOP, (char[]){ opChar, '\0' });
            addChild(arena, pair, sx);
            addChild(arena, pair, dx);
            leaves[writeIdx++] = pair;
        }
        // Carry over odd trailing leaf node to the next height level
        if (i < count) leaves[writeIdx++] = leaves[i];
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
    if (contains_float_literal(expr)) return expr; // Preserve IEEE 754 precision

    char opChar = expr->text[0];
    unsigned short opKey = OP_KEY(opChar, 0);

    ASTNode **leaves = NULL;
    int count = 0, cap = 0;

    flatten_chain(expr, opKey, &leaves, &count, &cap);
    ASTNode *result = build_balanced_tree(arena, leaves, count, opChar);

    free(leaves);
    return result;
}


// EXPRESSION OPTIMIZATION PASS HELPERS


/**
 * @brief Simplifies binary expressions based on algebraic identity laws.
 *
 * @return Pointer to simplified node, or `NULL` if no identity applies.
 */
static ASTNode *simplify_algebraic_identity(Arena *arena, unsigned short key,
                                            ASTNode *sx, ASTNode *dx) {
    switch (key) {
        case OP_KEY('+', 0):
            if (literal_int_equals(dx, 0)) return sx; // x + 0 -> x
            if (literal_int_equals(sx, 0)) return dx; // 0 + x -> x
            break;

        case OP_KEY('-', 0):
            if (literal_int_equals(dx, 0)) return sx; // x - 0 -> x
            break;

        case OP_KEY('*', 0):
            if (literal_int_equals(dx, 1)) return sx; // x * 1 -> x
            if (literal_int_equals(sx, 1)) return dx; // 1 * x -> x

            // x * 0 -> 0 (Valid only if operand has no side effects)
            if (literal_int_equals(dx, 0) && !has_side_effect(sx)) {
                return newNode(arena, ND_NUM_INT, "0");
            }
            if (literal_int_equals(sx, 0) && !has_side_effect(dx)) {
                return newNode(arena, ND_NUM_INT, "0");
            }
            break;

        case OP_KEY('&','&'):
            if (literal_int_equals(sx, 0)) return newNode(arena, ND_NUM_INT, "0"); // 0 && x -> 0
            if (literal_int_equals(sx, 1)) return dx;                               // 1 && x -> x
            break;

        case OP_KEY('|','|'):
            if (literal_int_equals(sx, 1)) return newNode(arena, ND_NUM_INT, "1"); // 1 || x -> 1
            if (literal_int_equals(sx, 0)) return dx;                               // 0 || x -> x
            break;

        default:
            break;
    }
    return NULL;
}

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

/**
 * @brief Handles rewrite, constant folding, algebraic identity simplification,
 *        and tree rebalancing for binary operation nodes.
 */
static ASTNode *rewrite_binop_expr(Arena *arena, ASTNode *expr) {
    expr->children[0] = rewrite_expr(arena, expr->children[0]);
    expr->children[1] = rewrite_expr(arena, expr->children[1]);
    ASTNode *sx = expr->children[0];
    ASTNode *dx = expr->children[1];

    // Fold binary operations on constant literal pair
    if (is_numeric_literal(sx) && is_numeric_literal(dx)) {
        ASTNode *folded = fold_binop_literals(arena, expr->text, sx, dx);
        if (folded) return folded;
    }

    // Algebraic Identities Simplifications
    unsigned short key = OP_KEY(expr->text[0], expr->text[1]);
    ASTNode *simplified = simplify_algebraic_identity(arena, key, sx, dx);
    if (simplified) return simplified;

    // Balance associative operator tree chains
    return balance_assoc_chain(arena, expr);
}

/**
 * @brief Recursively optimizes an AST expression node and its entire subtree.
 *
 * @param arena Pointer to the memory arena used for new node allocations.
 * @param expr  Pointer to the AST expression node to optimize.
 * @return Pointer to the optimized AST expression node (or a newly folded node).
 */
static ASTNode *rewrite_expr(Arena *arena, ASTNode *expr) {
    if (!expr) return NULL;

    switch (expr->kind) {

    case ND_NUM_INT:
    case ND_NUM_FLOAT:
    case ND_ID:
        return expr;

    case ND_UNARY:
        return rewrite_unary_expr(arena, expr);

    case ND_BINOP:
        return rewrite_binop_expr(arena, expr);

    case ND_ASSIGN:
        expr->children[0] = rewrite_expr(arena, expr->children[0]);
        expr->children[1] = rewrite_expr(arena, expr->children[1]);
        return expr;

    case ND_CALL:
        for (int i = 0; i < expr->nchildren; i++) {
            expr->children[i] = rewrite_expr(arena, expr->children[i]);
        }
        return expr;

    case ND_ARRAY_ACCESS:
        expr->children[0] = rewrite_expr(arena, expr->children[0]);
        return expr;

    default:
        return expr;
    }
}


// STATEMENT OPTIMIZATION HELPERS


/**
 * @brief Optimizes block statements by flattening nested blocks and removing dead statements.
 */
static ASTNode *rewrite_block_stmt(Arena *arena, ASTNode *stmt) {
    ASTNode **oldChildren = stmt->children;
    int oldCount = stmt->nchildren;

    stmt->children  = oldCount > 0 ? arena_alloc(arena, (size_t)oldCount * sizeof(ASTNode *)) : NULL;
    stmt->nchildren = 0;
    stmt->capacity  = oldCount;

    for (int i = 0; i < oldCount; i++) {
        ASTNode *result = rewrite_stmt(arena, oldChildren[i]);
        if (!result) continue;

        if (result->kind == ND_BLOCK) {
            for (int j = 0; j < result->nchildren; j++) {
                addChild(arena, stmt, result->children[j]);
            }
        } else {
            addChild(arena, stmt, result);
        }
    }
    return stmt;
}

/**
 * @brief Optimizes 'if' statements by evaluating constant conditions and pruning unreachable branches.
 */
static ASTNode *rewrite_if_stmt(Arena *arena, ASTNode *stmt) {
    stmt->children[0] = rewrite_expr(arena, stmt->children[0]);
    ASTNode *cond = stmt->children[0];

    // Prune unreachable branches when condition is known at compile time
    if (is_numeric_literal(cond)) {
        int condTrue    = !literal_is_zero(cond);
        ASTNode *thenBr = stmt->children[1];
        ASTNode *elseBr = (stmt->nchildren > 2) ? stmt->children[2] : NULL;

        ASTNode *survivor = condTrue ? thenBr : elseBr;
        return survivor ? rewrite_stmt(arena, survivor) : NULL;
    }

    // Optimize reachable branches for dynamic condition
    stmt->children[1] = rewrite_stmt(arena, stmt->children[1]);
    if (!stmt->children[1]) {
        stmt->children[1] = newNode(arena, ND_BLOCK, NULL);
    }

    if (stmt->nchildren > 2) {
        stmt->children[2] = rewrite_stmt(arena, stmt->children[2]);
        if (!stmt->children[2]) {
            stmt->children[2] = newNode(arena, ND_BLOCK, NULL);
        }
    }
    return stmt;
}

/**
 * @brief Optimizes 'while' loops, eliminating dead loops with constant zero conditions.
 */
static ASTNode *rewrite_while_stmt(Arena *arena, ASTNode *stmt) {
    stmt->children[0] = rewrite_expr(arena, stmt->children[0]);
    ASTNode *cond = stmt->children[0];

    // Eliminate while loop entirely if condition is constant zero (false)
    if (is_numeric_literal(cond) && literal_is_zero(cond)) {
        return NULL;
    }

    stmt->children[1] = rewrite_stmt(arena, stmt->children[1]);
    if (!stmt->children[1]) {
        stmt->children[1] = newNode(arena, ND_BLOCK, NULL);
    }
    return stmt;
}

/**
 * @brief Recursively optimizes an AST statement node, eliminating dead code and unnesting blocks.
 *
 * @param arena Pointer to the memory arena for node allocation.
 * @param stmt  Pointer to the AST statement node to optimize.
 * @return Pointer to the optimized statement node, a replacement node, or `NULL` if eliminated.
 */
static ASTNode *rewrite_stmt(Arena *arena, ASTNode *stmt) {
    if (!stmt) return NULL;

    switch (stmt->kind) {

    case ND_VAR_DECL:
        for (int i = 0; i < stmt->nchildren; i++) {
            stmt->children[i] = rewrite_expr(arena, stmt->children[i]);
        }
        return stmt;

    case ND_BLOCK: return rewrite_block_stmt(arena, stmt);

    case ND_IF: return rewrite_if_stmt(arena, stmt);

    case ND_WHILE: return rewrite_while_stmt(arena, stmt);

    case ND_RETURN:
    case ND_EXPR_STMT:
        stmt->children[0] = rewrite_expr(arena, stmt->children[0]);
        return stmt;

    default:
        return stmt;
    }
}

void optimize_ast(ASTNode *program, Arena *astArena) {
    if (!program) return;

    // Traverse all global declarations in the program root
    for (int i = 0; i < program->nchildren; i++) {
        ASTNode *decl = program->children[i];
        if (!decl) continue;

        if (decl->kind == ND_FUNC_DECL) {
            // Optimize function body statements
            ASTNode *body = decl->children[decl->nchildren - 1];
            ASTNode *optimizedBody = rewrite_stmt(astArena, body);
            if (!optimizedBody) {
                optimizedBody = newNode(astArena, ND_BLOCK, NULL);
            }
            decl->children[decl->nchildren - 1] = optimizedBody;

        } else if (decl->kind == ND_VAR_DECL) {
            // Optimize global variable initialization expressions
            for (int c = 0; c < decl->nchildren; c++) {
                decl->children[c] = rewrite_expr(astArena, decl->children[c]);
            }
        }
    }
}