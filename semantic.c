#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "semantic.h"
#include "ast_to_symtab.h"

static DataType checkExprType(ASTNode *expr, Scope *scope, int *errors);
static void     checkStmt(ASTNode *stmt, Scope *scope, DataType returnType,
                           Arena *arena, int *errors);


/**
 * @brief Centralized error reporting helper to eliminate duplicated boilerplate code.
 *
 * Increments the error counter and prints formatted error messages to standard error.
 *
 * @param errors Pointer to the error accumulator counter.
 * @param fmt    Format string (printf-style).
 * @param ...    Variadic arguments matching the format string.
 */
static void reportError(int *errors, const char *fmt, ...) {
    if (errors) (*errors)++;
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

/**
 * @brief Return a human-readable name for a DataType (used in error messages).
 * @param t  Data type to stringify.
 * @return   "int", "float", "void", or "?" for unknown values.
 */
static inline const char *typeName(DataType t) {
    switch (t) {
        case T_INT:   return "int";
        case T_FLOAT: return "float";
        case T_VOID:  return "void";
        default:      return "?";
    }
}

/**
 * @brief Check whether a value of type @p value is implicitly convertible
 *        to type @p target.
 *
 * The only implicit conversion allowed by the language is widening:
 * `int` → `float`.  The reverse (narrowing, `float` → `int`) and any
 * other combination are rejected.  This single predicate is reused for
 * assignments, function-argument checking, and return-type validation,
 * keeping the type rules consistent across all contexts.
 *
 * @param target  Declared type of the destination (lhs / parameter / return).
 * @param value   Type of the expression being supplied (rhs / argument).
 * @return        1 if the assignment is legal, 0 otherwise.
 */
static inline int isTypeCompatible(DataType target, DataType value) {
    if (target == value)                    return 1; // identity: always ok
    if (target == T_FLOAT && value == T_INT) return 1; // widening: int → float
    return 0;
}


/* =========================================================================
 * Name resolution
 * ========================================================================= */

/**
 * @brief Shared lookup and validation logic for ND_ID and ND_ARRAY_ACCESS.
 *
 * Both node kinds refer to a declared variable; they differ only in
 * whether they expect the symbol to be an array (@p wantArray).  Factoring
 * the lookup here avoids duplicating the "not declared / not a variable /
 * wrong array-ness" checks in two separate switch cases.
 *
 * On success the function stamps @p expr with the resolved coordinates
 * (scopeLevel, offset) so that ir_generate() can identify the variable
 * without performing another symbol-table traversal.
 *
 * @param expr       AST node whose @c text field holds the identifier name.
 * @param scope      Innermost active scope; lookup walks up to global.
 * @param wantArray  1 when called from ND_ARRAY_ACCESS, 0 from ND_ID.
 * @param outSym     If non-NULL, receives a copy of the resolved Symbol.
 * @param errors     Incremented once per error found.
 * @return           DataType of the symbol, or T_VOID on any error.
 */
static DataType resolveNameUse(ASTNode *expr, Scope *scope, int wantArray,
                              Symbol *outSym, int *errors) {
    Symbol sym;
    if (!sym_resolve(scope, expr->text, &sym)) {
        reportError(errors, "Errore: '%s' non e' stato dichiarato\n", expr->text);
        return T_VOID;
    }
    if (sym.kind != SYM_VAR) {
        // Function names are valid only in ND_CALL, not as plain identifiers.
        reportError(errors, "Errore: '%s' e' una funzione, non una variabile\n", expr->text);
        return T_VOID;
    }
    if (wantArray && !sym.isArray) {
        reportError(errors, "Errore: '%s' non e' un array, non si puo' usare con []\n", expr->text);
        return T_VOID;
    }
    if (!wantArray && sym.isArray) {
        // An array name used without [] would decay to a pointer in C, but
        // this language does not support pointer arithmetic — reject it.
        reportError(errors, "Errore: '%s' e' un array, va usato con [] e non da solo\n", expr->text);
        return T_VOID;
    }

    // Stamp resolution coordinates onto the node so ir_generate needs no re-lookup.
    expr->scopeLevel = sym.scopeLevel;
    expr->offset     = sym.offset;

    if (outSym) *outSym = sym;
    return sym.dataType;
}

/* =========================================================================
 * Compile-time constant evaluation (for static bounds checking)
 * ========================================================================= */

/**
 * @brief Try to evaluate @p expr as a compile-time integer constant.
 *
 * Handles:
 *   - Integer literals (ND_NUM_INT).
 *   - Unary minus applied to a constant: `-5` → -5.
 *   - Constant arithmetic binary expressions using `+`, `-`, `*`, `/`, `%`.
 *
 * Division and modulo by zero are treated as "not a constant" (returns 0)
 * rather than producing undefined behaviour.  Float literals and any
 * expression involving variables are also "not a constant".
 *
 * This is used exclusively by the ND_ARRAY_ACCESS checker to validate
 * indices at compile time; runtime indices cannot be checked statically.
 *
 * @param expr  Expression node to evaluate.
 * @param out   Receives the integer value when the function returns 1.
 * @return      1 if @p expr evaluates to a known constant integer, 0 otherwise.
 */
static int tryEvalConstant(ASTNode *expr, long *out) {
    if (expr->kind == ND_NUM_INT) {
        *out = atol(expr->text);
        return 1;
    }

    // Unary minus: recurse and negate.
    if (expr->kind == ND_UNARY && expr->text[0] == '-' && expr->text[1] == '\0') {
        long inner;
        if (tryEvalConstant(expr->children[0], &inner)) {
            *out = -inner;
            return 1;
        }
        return 0;
    }

    if (expr->kind == ND_BINOP) {
        long a, b;
        if (!tryEvalConstant(expr->children[0], &a)) return 0;
        if (!tryEvalConstant(expr->children[1], &b)) return 0;

        // All arithmetic operators are a single character: use direct switch.
        switch (expr->text[0]) {
        case '+': *out = a + b;                       return 1;
        case '-': *out = a - b;                       return 1;
        case '*': *out = a * b;                       return 1;
        case '/': if (b == 0) return 0; *out = a / b; return 1;
        case '%': if (b == 0) return 0; *out = a % b; return 1;
        default:  return 0; // comparison / logical operators: not folded here
        }
    }

    return 0; // variables, float literals, calls: not constant
}


/**
 * @brief Helper function to perform static array bounds checking.
 * 
 * @param expr     Array access AST node.
 * @param idx      Index AST node.
 * @param sym      Resolved array symbol.
 * @param errors   Error count accumulator.
 */
static void check_array_bounds(ASTNode *expr, ASTNode *idx, const Symbol *sym, int *errors) {
    long constValue;
    if (tryEvalConstant(idx, &constValue)) {
        if (constValue < 0 || constValue >= sym->arraySize) {
            reportError(errors, "Errore: indice %ld fuori dai limiti di '%s' (dimensione %d)\n",
                        constValue, expr->text, sym->arraySize);
        }
    }
}

/**
 * @brief Helper function to check arguments passed to a function call.
 * 
 * @param expr        ND_CALL AST node.
 * @param scope       Current scope.
 * @param sym         Resolved function symbol.
 * @param checkLimit  Number of arguments to type-check.
 * @param errors      Error count accumulator.
 */
static void check_call_arguments(ASTNode *expr, Scope *scope, const Symbol *sym, int checkLimit, int *errors) {
    for (int i = 0; i < expr->nchildren; i++) {
        DataType argType = checkExprType(expr->children[i], scope, errors);
        if (i < checkLimit) {
            DataType paramType = symtab_unpack_param_type(sym->paramTypes, i);
            if (argType != T_VOID && !isTypeCompatible(paramType, argType)) {
                reportError(errors, "Errore: argomento %d di '%s' e' %s, atteso %s\n",
                            i + 1, expr->text, typeName(argType), typeName(paramType));
            }
        }
    }
}

/**
 * @brief Helper function to validate variable initialization on declaration.
 * 
 * @param stmt     ND_VAR_DECL AST node.
 * @param scope    Current scope.
 * @param declType Resolved declared data type.
 * @param varName  Variable name.
 * @param isArray  Flag specifying if variable is an array.
 * @param arraySize Size of the array (if applicable).
 * @param errors   Error count accumulator.
 */
static void check_variable_init(ASTNode *stmt, Scope *scope, DataType declType,
                                const char *varName, int isArray, int arraySize, int *errors) {
    if (!isArray) {
        // Scalar initializer: exactly one expression child.
        DataType t = checkExprType(stmt->children[0], scope, errors);
        if (t != T_VOID && !isTypeCompatible(declType, t)) {
            reportError(errors, "Errore: non si puo' inizializzare '%s' (%s) con un valore %s\n",
                        varName, typeName(declType), typeName(t));
        }
        return;
    }

    // Array initializer list.
    if (stmt->nchildren > arraySize) {
        reportError(errors, "Errore: troppi inizializzatori per '%s' (%d forniti, dimensione %d)\n",
                    varName, stmt->nchildren, arraySize);
    }
    for (int i = 0; i < stmt->nchildren; i++) {
        DataType t = checkExprType(stmt->children[i], scope, errors);
        if (t != T_VOID && !isTypeCompatible(declType, t)) {
            reportError(errors, "Errore: elemento %d dell'inizializzatore di '%s' e' %s, atteso %s\n",
                        i, varName, typeName(t), typeName(declType));
        }
    }
}

/* =========================================================================
 * Expression type-checking
 * ========================================================================= */

/**
 * @brief Recursively type-check an expression and return its DataType.
 *
 * Resolves every identifier encountered by stamping (scopeLevel, offset)
 * onto ND_ID and ND_ARRAY_ACCESS nodes.  Returns T_VOID on error so that
 * callers can suppress follow-on diagnostics by checking for T_VOID before
 * reporting type-mismatch errors.
 *
 * @param expr    Expression node to check (must not be NULL).
 * @param scope   Innermost active scope at the point of this expression.
 * @param errors  Incremented once per semantic error found.
 * @return        Inferred DataType of the expression, or T_VOID on error.
 */
static DataType checkExprType(ASTNode *expr, Scope *scope, int *errors) {
    if (!expr) return T_VOID;

    switch (expr->kind) {

    case ND_NUM_INT:
        return T_INT;

    case ND_NUM_FLOAT:
        return T_FLOAT;

    case ND_ID:
        return resolveNameUse(expr, scope, 0, NULL, errors);

    /* ---- Array element access: arr[idx] --------------------------------- */
    case ND_ARRAY_ACCESS: {
        ASTNode *idx = expr->children[0];
        DataType idxType = checkExprType(idx, scope, errors);

        // Index must be integral; T_VOID means an earlier error already fired.
        if (idxType != T_VOID && idxType != T_INT) {
            reportError(errors, "Errore: l'indice di '%s[]' deve essere int, non %s\n",
                        expr->text, typeName(idxType));
        }

        Symbol sym;
        DataType elemType = resolveNameUse(expr, scope, /*wantArray=*/1, &sym, errors);

        if (elemType != T_VOID) {
            check_array_bounds(expr, idx, &sym, errors);
        }

        return elemType;
    }

    /* ---- Function call: f(arg0, arg1, ...) ------------------------------ */
    case ND_CALL: {
        Symbol sym;
        int found = sym_resolve(scope, expr->text, &sym);

        if (!found) {
            reportError(errors, "Errore: funzione '%s' non e' stata dichiarata\n", expr->text);
        } else if (sym.kind != SYM_FUNC) {
            reportError(errors, "Errore: '%s' non e' una funzione\n", expr->text);
            found = 0; // disable per-argument checking below
        } else if (expr->nchildren != sym.paramCount) {
            reportError(errors, "Errore: '%s' chiamata con %d argomenti, ne servono %d\n",
                        expr->text, expr->nchildren, sym.paramCount);
            // Do not set found=0: still type-check the arguments supplied.
        }

        // Type-check each argument even when the arity is wrong.
        int checkLimit = found ? sym.paramCount : 0;
        check_call_arguments(expr, scope, &sym, checkLimit, errors);

        return found ? sym.dataType : T_VOID;
    }

    /* ---- Assignment: lhs = rhs ------------------------------------------ */
    case ND_ASSIGN: {
        ASTNode *lvalue = expr->children[0];
        ASTNode *rvalue = expr->children[1];

        // The parser accepts "1 = 2;" syntactically — reject it here at the
        // semantic level where we have enough context to diagnose it clearly.
        if (lvalue->kind != ND_ID && lvalue->kind != ND_ARRAY_ACCESS) {
            reportError(errors, "Errore: il lato sinistro di '=' non e' una variabile valida\n");
            checkExprType(rvalue, scope, errors); // still visit rhs for further errors
            return T_VOID;
        }

        DataType lt = checkExprType(lvalue, scope, errors);
        DataType rt = checkExprType(rvalue, scope, errors);

        // T_VOID on either side means an error was already reported; skip the
        // type-mismatch check to avoid cascading "can't assign void to void".
        if (lt != T_VOID && rt != T_VOID && !isTypeCompatible(lt, rt)) {
            reportError(errors, "Errore: non si puo' assegnare %s a una variabile %s\n",
                        typeName(rt), typeName(lt));
        }
        return lt; // assignment expression has the type of the lhs
    }

    /* ---- Binary operators ------------------------------------------------ */
    case ND_BINOP: {
        DataType lt = checkExprType(expr->children[0], scope, errors);
        DataType rt = checkExprType(expr->children[1], scope, errors);

        // '%' requires integer operands on both sides (no float modulo).
        if (expr->text[0] == '%' && expr->text[1] == '\0') {
            if ((lt != T_VOID && lt != T_INT) || (rt != T_VOID && rt != T_INT)) {
                reportError(errors, "Errore: l'operatore %% richiede operandi interi\n");
            }
            return T_INT;
        }

        // Relational and logical operators always produce an int (boolean) result
        // regardless of the operand types.
        if (strchr("=!&|<>", expr->text[0]) != NULL) {
            return T_INT;
        }

        // Arithmetic operators (+, -, *, /):

        if (lt == T_VOID || rt == T_VOID) return T_VOID;
        if (lt == T_FLOAT || rt == T_FLOAT) return T_FLOAT;
        return T_INT;
    }

    /* ---- Unary operators ------------------------------------------------- */
    case ND_UNARY:
        // '!' always yields int (boolean); unary '-' preserves operand type.
        if (expr->text[0] == '!' && expr->text[1] == '\0') {
            checkExprType(expr->children[0], scope, errors);
            return T_INT;
        }
        return checkExprType(expr->children[0], scope, errors);

    default:
        reportError(errors, "Errore interno: nodo inatteso in un'espressione\n");
        return T_VOID;
    }
}


/**
 * @brief Recursively type-check a statement node.
 *
 * Handles all statement kinds produced by the parser.  ND_BLOCK creates a
 * fresh child scope so that variables declared inside are not visible
 * outside the block.  ND_VAR_DECL both registers the variable in the
 * current scope and validates any initializer.
 *
 * @param stmt        Statement node to check (may be NULL — silently ignored).
 * @param scope       Active scope at the point of this statement.
 * @param returnType  Return type of the enclosing function; threaded through
 *                    the recursion so ND_RETURN nodes can be validated.
 * @param arena       Scratch arena forwarded to declaration helpers
 *                    (st_bind_symbol, st_elaborate_decl).
 * @param errors      Incremented once per semantic error found.
 */
static void checkStmt(ASTNode *stmt, Scope *scope, DataType returnType,
                       Arena *arena, int *errors) {
    if (!stmt) return;

    switch (stmt->kind) {

    /* Variable declaration */
    case ND_VAR_DECL: {
        // Register the variable in the current scope and stamp the node with
        // (scopeLevel, offset); returns 0 on redeclaration.
        if (!st_bind_symbol(arena, scope, stmt)) {
            (*errors)++;
            break; // no initializer check if the declaration itself failed
        }

        if (stmt->nchildren == 0) break; // no initializer → nothing more to do

        // Parse the declaration text to retrieve the declared type for
        // initializer compatibility checking.
        char *typeNameBuf, *varName;
        int isArray, arraySize;
        st_elaborate_decl(arena, stmt->text, &typeNameBuf, &varName,
                                &isArray, &arraySize);
        DataType declType = st_resolve_type(typeNameBuf);

        check_variable_init(stmt, scope, declType, varName, isArray, arraySize, errors);
        break;
    }

    /* ---- Block: { stmt* } ----------------------------------------------- */
    case ND_BLOCK: {
        // Each block gets its own scope so that inner declarations shadow
        // outer ones but do not persist beyond the closing brace.
        Scope *blockScope = sym_scopeCreate(scope);
        for (int i = 0; i < stmt->nchildren; i++)
            checkStmt(stmt->children[i], blockScope, returnType, arena, errors);
        // blockScope remains alive as a child of scope; freed by
        // sym_finalize() at the end of compilation.
        break;
    }

    /* ---- if / if-else ---------------------------------------------------- */
    case ND_IF:
        checkExprType(stmt->children[0], scope, errors);                         // condition
        checkStmt(stmt->children[1], scope, returnType, arena, errors);      // then-branch
        if (stmt->nchildren > 2)
            checkStmt(stmt->children[2], scope, returnType, arena, errors);  // else-branch
        break;

    /* ---- while ----------------------------------------------------------- */
    case ND_WHILE:
        checkExprType(stmt->children[0], scope, errors);                     // loop condition
        checkStmt(stmt->children[1], scope, returnType, arena, errors);  // loop body
        break;

    /* ---- return expr; ---------------------------------------------------- */
    case ND_RETURN: {
        DataType t = checkExprType(stmt->children[0], scope, errors);
        if (t != T_VOID && !isTypeCompatible(returnType, t)) {
            reportError(errors, "Errore: return di tipo %s non compatibile col tipo %s della funzione\n",
                        typeName(t), typeName(returnType));
        }
        break;
    }

    /* ---- Expression statement: call(), assignment as statement, etc. ----- */
    case ND_EXPR_STMT:
        checkExprType(stmt->children[0], scope, errors);
        break;

    default:
        // ND_FUNC_DECL nested inside a body is not part of the language;
        // ND_ERROR nodes from parser recovery are silently ignored here.
        break;
    }
}


/**
 * @brief Type-check a single function declaration end-to-end.
 *
 * Creates a function-level scope as a direct child of @p global, declares
 * all parameters in it, then hands off the body block to checkStmt().
 * The declared return type is extracted from the declaration text and
 * threaded into the recursive walk so that ND_RETURN nodes can be
 * validated against it.
 *
 * Note: the function scope is a child of @p global (not of some
 * intermediate "file" scope) so that the function body can look up both
 * its own parameters and all top-level names without any extra hops.
 *
 * @param decl    ND_FUNC_DECL node.  All children except the last are
 *                ND_PARAM nodes; the last child is always the body ND_BLOCK.
 * @param global  Global scope; parent of the new function-level scope.
 * @param arena   Scratch arena for declaration parsing (caller owns it).
 * @param errors  Incremented for each semantic error found.
 */
static void checkFunctionBody(ASTNode *decl, Scope *global,
                               Arena *arena, int *errors) {
    // Extract return type and function name from the declaration text
    // (format: "retType funcName", e.g. "int main" or "float compute").
    char *typeNameBuf, *funcName;
    int isArray, arraySize;
    st_elaborate_decl(arena, decl->text, &typeNameBuf, &funcName,
                            &isArray, &arraySize);

    DataType returnType = st_resolve_type(typeNameBuf);

    // Parameter scope is a direct child of global so forward references to
    // other top-level functions are visible from inside the body.
    Scope *fnScope = sym_scopeCreate(global);

    // All children except the last are parameters (ND_PARAM nodes).
    int paramCount = decl->nchildren - 1;
    for (int p = 0; p < paramCount; p++) {
        if (!st_bind_symbol(arena, fnScope, decl->children[p]))
            (*errors)++;
    }

    // The last child is always the body block.
    ASTNode *body = decl->children[decl->nchildren - 1];
    checkStmt(body, fnScope, returnType, arena, errors);
}


int semantic_check(ASTNode *program, Scope *global) {
    int errors = 0;

    // One arena for all scratch allocations during the entire walk.
    // Destroyed at the end: no per-allocation free needed.
    Arena *arena = arena_create(0);

    for (int i = 0; i < program->nchildren; i++) {
        ASTNode *decl = program->children[i];
        // Global variable declarations were fully handled by Pass 1
        // (st_resolve_global_namespace); only function bodies need walking here.
        if (decl->kind != ND_FUNC_DECL) continue;
        checkFunctionBody(decl, global, arena, &errors);
    }

    arena_destroy(arena);
    return errors;
}