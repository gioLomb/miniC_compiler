#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <float.h>
#include "optimize.h"

// Calculate maximum buffer size required to safely convert numeric values into string representation
#define INT_BUF_SIZE   (CHAR_BIT * sizeof(long) / 3 + 3)
#define FLOAT_BUF_SIZE (DECIMAL_DIG + 8)
#define NUM_BUF_SIZE   (FLOAT_BUF_SIZE > INT_BUF_SIZE ? FLOAT_BUF_SIZE : INT_BUF_SIZE)

// ============================================================================
// LITERAL HELPER FUNCTIONS
// ============================================================================

static inline int isIntLiteral(ASTNode *n)     { return n && n->kind == ND_NUM_INT; }
static inline int isFloatLiteral(ASTNode *n)   { return n && n->kind == ND_NUM_FLOAT; }
static inline int isNumericLiteral(ASTNode *n) { return isIntLiteral(n) || isFloatLiteral(n); }

static inline long   literalAsLong(ASTNode *n)   { return atol(n->text); }
static inline double literalAsDouble(ASTNode *n) { return atof(n->text); }
static inline int    literalIsZero(ASTNode *n)   { return literalAsDouble(n) == 0.0; }

static inline int literalIntEquals(ASTNode *n, long v) {
    return isIntLiteral(n) && literalAsLong(n) == v;
}

/**
 * @brief Deallocates an AST node container without freeing its text buffer.
 *
 * @details Frees the array of child pointers and the ASTNode structure itself.
 *          The `text` field is allocated inside the persistent memory arena and must not be freed individually.
 *
 * @param node Pointer to the AST node to be shallow-freed.
 */
static void freeNodeShallow(ASTNode *node) {
    if (!node) return;
    free(node->children);
    free(node);
}

// ============================================================================
// SIDE-EFFECT ANALYSIS
// ============================================================================

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
static int hasSideEffect(ASTNode *expr) {
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
    for (int i = 0; i < expr->nchildren; i++)
        if (hasSideEffect(expr->children[i])) return 1;

    return 0;
}

// ============================================================================
// COMPILE-TIME CONSTANT FOLDING
// ============================================================================

/**
 * @brief Evaluates a binary operation on two numeric literal AST nodes at compile time.
 *
 * @details Computes the result of applying a binary operator (`+`, `-`, `*`, `/`, `%`, 
 *          `==`, `!=`, `<`, `>`, `<=`, `>=`, `&&`, `||`) to two literal operands (`ND_NUM_INT` 
 *          or `ND_NUM_FLOAT`). Performs appropriate type promotion, handling mixed-type 
 *          operations, and safeguards against compile-time crashes (such as integer/float 
 *          division or modulo by zero).
 *
 * @param arena Pointer to the memory arena used to allocate the new folded result node.
 * @param op    Null-terminated string representing the binary operator.
 * @param sx    Pointer to the left-hand operand AST node (must be numeric literal).
 * @param dx    Pointer to the right-hand operand AST node (must be numeric literal).
 * @return Pointer to a new `ND_NUM_INT` or `ND_NUM_FLOAT` AST node containing the folded result,
 *         or `NULL` if folding cannot be performed safely (e.g., division by zero).
 */
static ASTNode *foldBinopLiterals(Arena *arena,
                                   const char *op, ASTNode *sx, ASTNode *dx) {
    int bothInt = isIntLiteral(sx) && isIntLiteral(dx);
    char buf[NUM_BUF_SIZE];
    unsigned short key = OP_KEY(op[0], op[1]);

    // Evaluate comparison and logical operators (always return an integer boolean 0 or 1)
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
            return newNode(arena, ND_NUM_INT, buf);
        }
        default: break;
    }

    // Fold integer modulo operation while preventing compile-time divide-by-zero crash
    if (key == OP_KEY('%', 0)) {
        long b = literalAsLong(dx);
        if (b == 0) return NULL; // Abort folding to defer exception to runtime
        snprintf(buf, sizeof(buf), "%ld", literalAsLong(sx) % b);
        return newNode(arena, ND_NUM_INT, buf);
    }

    // Fold division operation while guarding against division by zero
    if (key == OP_KEY('/', 0)) {
        if (bothInt) {
            long b = literalAsLong(dx);
            if (b == 0) return NULL; // Abort folding
            snprintf(buf, sizeof(buf), "%ld", literalAsLong(sx) / b);
            return newNode(arena, ND_NUM_INT, buf);
        }
        double b = literalAsDouble(dx);
        if (b == 0.0) return NULL; // Abort folding
        snprintf(buf, sizeof(buf), "%g", literalAsDouble(sx) / b);
        return newNode(arena, ND_NUM_FLOAT, buf);
    }

    // Fold integer arithmetic operations (+, -, *)
    if (bothInt) {
        long a = literalAsLong(sx), b = literalAsLong(dx), r;
        switch (key) {
            case OP_KEY('+', 0): r = a + b; break;
            case OP_KEY('-', 0): r = a - b; break;
            default:             r = a * b; break;
        }
        snprintf(buf, sizeof(buf), "%ld", r);
        return newNode(arena, ND_NUM_INT, buf);
    }

    // Fold floating-point arithmetic operations
    double a = literalAsDouble(sx), b = literalAsDouble(dx), r;
    switch (key) {
        case OP_KEY('+', 0): r = a + b; break;
        case OP_KEY('-', 0): r = a - b; break;
        default:             r = a * b; break;
    }
    snprintf(buf, sizeof(buf), "%g", r);
    return newNode(arena, ND_NUM_FLOAT, buf);
}

/**
 * @brief Folds a unary operation (negation or logical NOT) on a literal.
 *
 * @param arena Pointer to the memory arena used for node allocation.
 * @param op    Operator character string (`-` or `!`).
 * @param child Pointer to the literal child operand AST node.
 * @return Pointer to a new folded AST node, or `NULL` if not foldable.
 */
static ASTNode *foldUnaryLiteral(Arena *arena, const char *op, ASTNode *child) {
    char buf[NUM_BUF_SIZE];

    // Arithmetic negation (-)
    if (op[0] == '-') {
        if (isIntLiteral(child)) {
            snprintf(buf, sizeof(buf), "%ld", -literalAsLong(child));
            return newNode(arena, ND_NUM_INT, buf);
        }
        snprintf(buf, sizeof(buf), "%g", -literalAsDouble(child));
        return newNode(arena, ND_NUM_FLOAT, buf);
    }

    // Logical NOT (!)
    if (op[0] == '!') {
        snprintf(buf, sizeof(buf), "%d", literalIsZero(child) ? 1 : 0);
        return newNode(arena, ND_NUM_INT, buf);
    }
    return NULL;
}

// ============================================================================
// TREE HEIGHT BALANCING FOR ASSOCIATIVE OPERATORS
// ============================================================================

/**
 * @brief Checks if a subtree contains floating-point literals.
 *
 * @details Floating-point reordering is disabled to preserve strict IEEE 754 precision semantics.
 *
 * @param n Pointer to the AST node to check.
 * @return 1 if floating-point literals exist in the subtree; 0 otherwise.
 */
static int containsFloatLiteral(ASTNode *n) {
    if (!n) return 0;
    if (n->kind == ND_NUM_FLOAT) return 1;
    for (int i = 0; i < n->nchildren; i++)
        if (containsFloatLiteral(n->children[i])) return 1;
    return 0;
}

/**
 * @brief Recursively flattens a chain of identical associative binary operations into a flat leaf array.
 *
 * @details Traverses a potentially deeply nested, unbalanced binary expression subtree 
 *          containing the same associative operator (e.g., `a + (b + (c + d))`). It extracts 
 *          all non-operator operand subtrees (leaves) into a linear dynamically-allocated array 
 *          while shallow-freeing intermediate binary node wrappers to avoid memory leaks.
 *
 * @param node   Pointer to the current AST node in the associative chain.
 * @param opKey  16-bit key representing the target associative operator to flatten.
 * @param leaves Double pointer to a dynamically allocated array of leaf AST node pointers.
 * @param count  Pointer to the current count of collected leaf nodes in the array.
 * @param cap    Pointer to the current capacity of the `leaves` array.
 */
static void flattenChain(ASTNode *node, unsigned short opKey,
                          ASTNode ***leaves, int *count, int *cap) {
    if (node->kind == ND_BINOP &&
        OP_KEY(node->text[0], node->text[1]) == opKey) {
        flattenChain(node->children[0], opKey, leaves, count, cap);
        flattenChain(node->children[1], opKey, leaves, count, cap);
        freeNodeShallow(node); // Discard internal binary node wrappers
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
 * @details Constructs a tree with minimal height (O(log N)) using a pairwise reduction 
 *          approach across the provided array of leaf nodes. During tree construction, adjacent 
 *          literal leaves are immediately folded to compress constant terms efficiently.
 *
 * @param arena    Pointer to the memory arena used for allocating new binary nodes.
 * @param leaves   Array of pointers to AST leaf subtrees.
 * @param count    Number of valid leaf entries in the array.
 * @param opChar   Operator character (`+` or `*`) used for connecting the pairs.
 * @return Pointer to the root node of the freshly built, height-balanced AST subtree.
 */
static ASTNode *buildBalanced(Arena *arena,
                               ASTNode **leaves, int count, char opChar) {
    while (count > 1) {
        int writeIdx = 0, i = 0;
        for (; i + 1 < count; i += 2) {
            ASTNode *sx = leaves[i];
            ASTNode *dx = leaves[i + 1];

            // Attempt immediate constant folding on adjacent literal pair
            if (isNumericLiteral(sx) && isNumericLiteral(dx)) {
                char opStr[3] = { opChar, '\0', '\0' };
                ASTNode *folded = foldBinopLiterals(arena, opStr, sx, dx);
                if (folded) {
                    freeAST(sx); freeAST(dx);
                    leaves[writeIdx++] = folded;
                    continue;
                }
            }

            // Construct balanced binary node pair
            ASTNode *pair = newNode(arena, ND_BINOP,
                                    (char[]){ opChar, '\0' });
            addChild(pair, sx);
            addChild(pair, dx);
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
 * @details Entry point for tree height reduction on associative operations. To ensure compliance
 *          with IEEE 754 floating-point standards (where floating-point addition and multiplication 
 *          are non-associative due to rounding errors), this optimization is strictly restricted 
 *          to integer expressions.
 *
 * @param arena Pointer to the memory arena for node allocation.
 * @param expr  Pointer to the root node of the binary operation expression to balance.
 * @return Pointer to the rebalanced (and possibly partially folded) AST expression root,
 *         or the original node if rebalancing is inapplicable.
 */
static ASTNode *balanceAssocChain(Arena *arena, ASTNode *expr) {
    if (expr->text[0] != '+' && expr->text[0] != '*') return expr;
    if (expr->text[1] != '\0') return expr;
    if (containsFloatLiteral(expr)) return expr; // Preserve IEEE 754 precision

    char opChar = expr->text[0];
    unsigned short opKey = OP_KEY(opChar, 0);

    ASTNode **leaves = NULL;
    int count = 0, cap = 0;

    flattenChain(expr, opKey, &leaves, &count, &cap);
    ASTNode *result = buildBalanced(arena, leaves, count, opChar);

    free(leaves);
    return result;
}

// ============================================================================
// EXPRESSION OPTIMIZATION PASS
// ============================================================================

/**
 * @brief Recursively optimizes an AST expression node and its entire subtree.
 *
 * @details Applies a series of algebraic and structural transformations:
 *          1. Recursive bottom-up optimization of child nodes.
 *          2. Unary and binary constant folding for literal operands.
 *          3. Simplification using algebraic identity laws (e.g., `x + 0 -> x`, 
 *             `x * 1 -> x`, `x - 0 -> x`).
 *          4. Zero-property multiplication elimination (`x * 0 -> 0`), validated via 
 *             `hasSideEffect()` to prevent discarding necessary runtime computations.
 *          5. Short-circuit logical simplification (`0 && x -> 0`, `1 || x -> 1`).
 *          6. Rebalancing of associative tree chains (+ and *) to reduce depth.
 *
 * @param arena Pointer to the memory arena used for new node allocations.
 * @param expr  Pointer to the AST expression node to optimize.
 * @return Pointer to the optimized AST expression node (or a newly folded node).
 */
static ASTNode *rewriteExpr(Arena *arena, ASTNode *expr) {
    if (!expr) return NULL;

    switch (expr->kind) {

    case ND_NUM_INT:
    case ND_NUM_FLOAT:
    case ND_ID:
        return expr;

    case ND_UNARY: {
        expr->children[0] = rewriteExpr(arena, expr->children[0]);
        ASTNode *child = expr->children[0];

        // Fold constant unary operations
        if (isNumericLiteral(child)) {
            ASTNode *folded = foldUnaryLiteral(arena, expr->text, child);
            if (folded) { freeAST(expr); return folded; }
        }
        return expr;
    }

    case ND_BINOP: {
        expr->children[0] = rewriteExpr(arena, expr->children[0]);
        expr->children[1] = rewriteExpr(arena, expr->children[1]);
        ASTNode *sx = expr->children[0];
        ASTNode *dx = expr->children[1];

        // 1. Fold binary operations on constant literal pair
        if (isNumericLiteral(sx) && isNumericLiteral(dx)) {
            ASTNode *folded = foldBinopLiterals(arena, expr->text, sx, dx);
            if (folded) { freeAST(expr); return folded; }
        }

        // 2. Algebraic Identities Simplifications
        unsigned short key = OP_KEY(expr->text[0], expr->text[1]);
        switch (key) {
            case OP_KEY('+', 0):
                if (literalIntEquals(dx, 0)) { freeNodeShallow(dx); freeNodeShallow(expr); return sx; } // x + 0 -> x
                if (literalIntEquals(sx, 0)) { freeNodeShallow(sx); freeNodeShallow(expr); return dx; } // 0 + x -> x
                break;
            case OP_KEY('-', 0):
                if (literalIntEquals(dx, 0)) { freeNodeShallow(dx); freeNodeShallow(expr); return sx; } // x - 0 -> x
                break;
            case OP_KEY('*', 0):
                if (literalIntEquals(dx, 1)) { freeNodeShallow(dx); freeNodeShallow(expr); return sx; } // x * 1 -> x
                if (literalIntEquals(sx, 1)) { freeNodeShallow(sx); freeNodeShallow(expr); return dx; } // 1 * x -> x

                // x * 0 -> 0 (Valid only if x has no side effects)
                if (literalIntEquals(dx, 0) && !hasSideEffect(sx)) {
                    freeAST(expr); return newNode(arena, ND_NUM_INT, "0");
                }
                if (literalIntEquals(sx, 0) && !hasSideEffect(dx)) {
                    freeAST(expr); return newNode(arena, ND_NUM_INT, "0");
                }
                break;
            case OP_KEY('&','&'):
                if (literalIntEquals(sx, 0)) { freeAST(expr); return newNode(arena, ND_NUM_INT, "0"); } // 0 && x -> 0
                if (literalIntEquals(sx, 1)) { freeNodeShallow(sx); freeNodeShallow(expr); return dx; } // 1 && x -> x
                break;
            case OP_KEY('|','|'):
                if (literalIntEquals(sx, 1)) { freeAST(expr); return newNode(arena, ND_NUM_INT, "1"); } // 1 || x -> 1
                if (literalIntEquals(sx, 0)) { freeNodeShallow(sx); freeNodeShallow(expr); return dx; } // 0 || x -> x
                break;
            default:
                break;
        }

        // 3. Balance associative operator tree chains
        return balanceAssocChain(arena, expr);
    }

    case ND_ASSIGN:
        expr->children[0] = rewriteExpr(arena, expr->children[0]);
        expr->children[1] = rewriteExpr(arena, expr->children[1]);
        return expr;

    case ND_CALL:
        for (int i = 0; i < expr->nchildren; i++)
            expr->children[i] = rewriteExpr(arena, expr->children[i]);
        return expr;

    case ND_ARRAY_ACCESS:
        expr->children[0] = rewriteExpr(arena, expr->children[0]);
        return expr;

    default:
        return expr;
    }
}

// ============================================================================
// STATEMENT OPTIMIZATION & DEAD CODE ELIMINATION PASS
// ============================================================================

/**
 * @brief Recursively optimizes an AST statement node, eliminating dead code and unnesting blocks.
 *
 * @details Traverses control flow and statement constructs to perform high-level structural cleanup:
 *          - **Variable Declarations**: Optimizes initializer expressions.
 *          - **Blocks**: Recursively optimizes statements and flattens nested block structures.
 *          - **If Statements**: Evaluates conditions at compile time. If the condition is constant, 
 *            prunes the unreachable branch completely and replaces the `if` node with the surviving branch.
 *          - **While Loops**: Removes `while(0)` loops entirely if the condition evaluates to constant false.
 *          - **Return / Expression Statements**: Optimizes attached child expressions.
 *
 * @param arena Pointer to the memory arena for node allocation.
 * @param stmt  Pointer to the AST statement node to optimize.
 * @return Pointer to the optimized statement node, a replacement node, or `NULL` if eliminated.
 */
static ASTNode *rewriteStmt(Arena *arena, ASTNode *stmt) {
    if (!stmt) return NULL;

    switch (stmt->kind) {

    case ND_VAR_DECL:
        for (int i = 0; i < stmt->nchildren; i++)
            stmt->children[i] = rewriteExpr(arena, stmt->children[i]);
        return stmt;

    case ND_BLOCK: {
        ASTNode **oldChildren = stmt->children;
        int oldCount = stmt->nchildren;

        stmt->children  = oldCount > 0 ? malloc((size_t)oldCount * sizeof(ASTNode *)) : NULL;
        stmt->nchildren = 0;
        stmt->capacity  = oldCount;

        // Optimize statements and flatten nested block structures
        for (int i = 0; i < oldCount; i++) {
            ASTNode *result = rewriteStmt(arena, oldChildren[i]);
            if (!result) continue;

            if (result->kind == ND_BLOCK) {
                // In-place inline unnesting of child block statements
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
        stmt->children[0] = rewriteExpr(arena, stmt->children[0]);
        ASTNode *cond = stmt->children[0];

        // Prune unreachable branches when condition is known at compile time
        if (isNumericLiteral(cond)) {
            int condTrue  = !literalIsZero(cond);
            ASTNode *thenBr = stmt->children[1];
            ASTNode *elseBr = (stmt->nchildren > 2) ? stmt->children[2] : NULL;

            ASTNode *survivor   = condTrue ? thenBr : elseBr;
            ASTNode *deadBranch = condTrue ? elseBr : thenBr;

            freeAST(cond);
            if (deadBranch) freeAST(deadBranch); // Prune dead code branch
            freeNodeShallow(stmt);

            return survivor ? rewriteStmt(arena, survivor) : NULL;
        }

        // Optimize reachable branches for dynamic condition
        stmt->children[1] = rewriteStmt(arena, stmt->children[1]);
        if (!stmt->children[1]) stmt->children[1] = newNode(arena, ND_BLOCK, NULL);

        if (stmt->nchildren > 2) {
            stmt->children[2] = rewriteStmt(arena, stmt->children[2]);
            if (!stmt->children[2]) stmt->children[2] = newNode(arena, ND_BLOCK, NULL);
        }
        return stmt;
    }

    case ND_WHILE: {
        stmt->children[0] = rewriteExpr(arena, stmt->children[0]);
        ASTNode *cond = stmt->children[0];

        // Eliminate while loop entirely if condition is constant zero (false)
        if (isNumericLiteral(cond) && literalIsZero(cond)) {
            freeAST(stmt); 
            return NULL;
        }

        stmt->children[1] = rewriteStmt(arena, stmt->children[1]);
        if (!stmt->children[1]) stmt->children[1] = newNode(arena, ND_BLOCK, NULL);
        return stmt;
    }

    case ND_RETURN:
    case ND_EXPR_STMT:
        stmt->children[0] = rewriteExpr(arena, stmt->children[0]);
        return stmt;

    default:
        return stmt;
    }
}

// ============================================================================
// MAIN ENTRY POINT
// ============================================================================

void optimize_ast(ASTNode *program, Arena *astArena) {
    if (!program) return;

    // Traverse all global declarations in the program root
    for (int i = 0; i < program->nchildren; i++) {
        ASTNode *decl = program->children[i];
        if (!decl) continue;

        if (decl->kind == ND_FUNC_DECL) {
            // Optimize function body statements
            ASTNode *body = decl->children[decl->nchildren - 1];
            ASTNode *optimizedBody = rewriteStmt(astArena, body);
            if (!optimizedBody) optimizedBody = newNode(astArena, ND_BLOCK, NULL);
            decl->children[decl->nchildren - 1] = optimizedBody;

        } else if (decl->kind == ND_VAR_DECL) {
            // Optimize global variable initialization expressions
            for (int c = 0; c < decl->nchildren; c++)
                decl->children[c] = rewriteExpr(astArena, decl->children[c]);
        }
    }
}
