#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "semantic.h"
#include "ast_to_symtab.h"
#include "parser/errorCollector.h"

static DataType check_expr_type(ASTNode *expr, Scope *scope, int *errors);
static DataType check_expr_type_impl(ASTNode *expr, Scope *scope, int *errors);
static void     check_stmt(ASTNode *stmt, Scope *scope, DataType returnType,
                           Arena *arena, int *errors);


/**
 * @brief Local counting wrapper around the shared error collector.
 *
 * Delegates print + global running total to ec_reportv() (error_collector.h),
 * removing the print/vfprintf duplication this function used to own.
 * Keeps only the per-call *errors accumulation, since semantic_check()'s
 * return value (and every caller checking pass1Errors/semErrors) depends
 * on a locally-scoped count, not just the global total.
 *
 * @param errors Pointer to the error accumulator counter (may be NULL).
 * @param fmt    Format string (printf-style).
 * @param ...    Variadic arguments matching the format string.
 */
static void report_error(int *errors, const char *fmt, ...) {
    if (errors) (*errors)++;
    va_list args;
    va_start(args, fmt);
    ec_reportv(fmt, args);
    va_end(args);
}

/**
 * @brief Return a human-readable name for a DataType (used in error messages).
 * @param t  Data type to stringify.
 * @return   "int", "float", "void", or "?" for unknown values.
 */
static inline const char *type_name(DataType t) {
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
static inline int is_type_compatible(DataType target, DataType value) {
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
static DataType resolve_name_use(ASTNode *expr, Scope *scope, int wantArray,
                              Symbol *outSym, int *errors) {
    Symbol sym;
    if (!sym_resolve(scope, expr->text, &sym)) {
        report_error(errors, "Errore: '%s' non e' stato dichiarato\n", expr->text);
        return T_VOID;
    }
    if (sym.kind != SYM_VAR) {
        // Function names are valid only in ND_CALL, not as plain identifiers.
        report_error(errors, "Errore: '%s' e' una funzione, non una variabile\n", expr->text);
        return T_VOID;
    }
    if (wantArray && !sym.isArray) {
        report_error(errors, "Errore: '%s' non e' un array, non si puo' usare con []\n", expr->text);
        return T_VOID;
    }
    if (!wantArray && sym.isArray) {
        // An array name used without [] would decay to a pointer in C, but
        // this language does not support pointer arithmetic — reject it.
        report_error(errors, "Errore: '%s' e' un array, va usato con [] e non da solo\n", expr->text);
        return T_VOID;
    }

    // Stamp resolution coordinates onto the node so ir_generate needs no re-lookup.
    expr->scopeLevel = sym.scopeLevel;
    expr->offset     = sym.offset;

    if (outSym) *outSym = sym;
    return sym.dataType;
}


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
static int try_eval_constant(ASTNode *expr, long *out) {
    if (expr->kind == ND_NUM_INT) {
        *out = atol(expr->text);
        return 1;
    }

    // Unary minus: recurse and negate.
    if (expr->kind == ND_UNARY && expr->text[0] == '-' && expr->text[1] == '\0') {
        long inner;
        if (try_eval_constant(expr->children[0], &inner)) {
            *out = -inner;
            return 1;
        }
        return 0;
    }

    if (expr->kind == ND_BINOP) {
        long a, b;
        if (!try_eval_constant(expr->children[0], &a)) return 0;
        if (!try_eval_constant(expr->children[1], &b)) return 0;

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
    if (try_eval_constant(idx, &constValue)) {
        if (constValue < 0 || constValue >= sym->arraySize) {
            report_error(errors, "Errore: indice %ld fuori dai limiti di '%s' (dimensione %d)\n",
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
        DataType argType = check_expr_type(expr->children[i], scope, errors);
        if (i < checkLimit) {
            DataType paramType = symtab_unpack_param_type(sym->paramTypes, i);
            if (argType != T_VOID && !is_type_compatible(paramType, argType)) {
                report_error(errors, "Errore: argomento %d di '%s' e' %s, atteso %s\n",
                            i + 1, expr->text, type_name(argType), type_name(paramType));
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
        DataType t = check_expr_type(stmt->children[0], scope, errors);
        if (t != T_VOID && !is_type_compatible(declType, t)) {
            report_error(errors, "Errore: non si puo' inizializzare '%s' (%s) con un valore %s\n",
                        varName, type_name(declType), type_name(t));
        }
        return;
    }

    // Array initializer list.
    if (stmt->nchildren > arraySize) {
        report_error(errors, "Errore: troppi inizializzatori per '%s' (%d forniti, dimensione %d)\n",
                    varName, stmt->nchildren, arraySize);
    }
    for (int i = 0; i < stmt->nchildren; i++) {
        DataType t = check_expr_type(stmt->children[i], scope, errors);
        if (t != T_VOID && !is_type_compatible(declType, t)) {
            report_error(errors, "Errore: elemento %d dell'inizializzatore di '%s' e' %s, atteso %s\n",
                        i, varName, type_name(t), type_name(declType));
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
static DataType check_expr_type_impl(ASTNode *expr, Scope *scope, int *errors) {
    if (!expr) return T_VOID;

    switch (expr->kind) {

    case ND_NUM_INT:
        return T_INT;

    case ND_NUM_FLOAT:
        return T_FLOAT;

    case ND_ID:
        return resolve_name_use(expr, scope, 0, NULL, errors);

    /* ---- Array element access: arr[idx] --------------------------------- */
    case ND_ARRAY_ACCESS: {
        ASTNode *idx = expr->children[0];
        DataType idxType = check_expr_type(idx, scope, errors);

        // Index must be integral; T_VOID means an earlier error already fired.
        if (idxType != T_VOID && idxType != T_INT) {
            report_error(errors, "Errore: l'indice di '%s[]' deve essere int, non %s\n",
                        expr->text, type_name(idxType));
        }

        Symbol sym;
        DataType elemType = resolve_name_use(expr, scope, /*wantArray=*/1, &sym, errors);

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
            report_error(errors, "Errore: funzione '%s' non e' stata dichiarata\n", expr->text);
        } else if (sym.kind != SYM_FUNC) {
            report_error(errors, "Errore: '%s' non e' una funzione\n", expr->text);
            found = 0; // disable per-argument checking below
        } else if (expr->nchildren != sym.paramCount) {
            report_error(errors, "Errore: '%s' chiamata con %d argomenti, ne servono %d\n",
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
            report_error(errors, "Errore: il lato sinistro di '=' non e' una variabile valida\n");
            check_expr_type(rvalue, scope, errors); // still visit rhs for further errors
            return T_VOID;
        }

        DataType lt = check_expr_type(lvalue, scope, errors);
        DataType rt = check_expr_type(rvalue, scope, errors);

        // T_VOID on either side means an error was already reported; skip the
        // type-mismatch check to avoid cascading "can't assign void to void".
        if (lt != T_VOID && rt != T_VOID && !is_type_compatible(lt, rt)) {
            report_error(errors, "Errore: non si puo' assegnare %s a una variabile %s\n",
                        type_name(rt), type_name(lt));
        }
        return lt; // assignment expression has the type of the lhs
    }

    /* ---- Binary operators ------------------------------------------------ */
    case ND_BINOP: {
        DataType lt = check_expr_type(expr->children[0], scope, errors);
        DataType rt = check_expr_type(expr->children[1], scope, errors);

        // '%' requires integer operands on both sides (no float modulo).
        if (expr->text[0] == '%' && expr->text[1] == '\0') {
            if ((lt != T_VOID && lt != T_INT) || (rt != T_VOID && rt != T_INT)) {
                report_error(errors, "Errore: l'operatore %% richiede operandi interi\n");
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
            check_expr_type(expr->children[0], scope, errors);
            return T_INT;
        }
        return check_expr_type(expr->children[0], scope, errors);

    default:
        report_error(errors, "Errore interno: nodo inatteso in un'espressione\n");
        return T_VOID;
    }
}

/** Stamps the resolved type onto @p expr so later IR/codegen stages
 *  (ir_mk_var, instr_selector's float dispatch) don't need to re-derive it. */
static DataType check_expr_type(ASTNode *expr, Scope *scope, int *errors) {
    DataType t = check_expr_type_impl(expr, scope, errors);
    if (expr) expr->dataType = t;
    return t;
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
static void check_stmt(ASTNode *stmt, Scope *scope, DataType returnType,
                       Arena *arena, int *errors) {
    if (!stmt) return;

    switch (stmt->kind) {

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
        char *type_nameBuf, *varName;
        int isArray, arraySize;
        st_elaborate_decl(arena, stmt->text, &type_nameBuf, &varName,
                                &isArray, &arraySize);
        DataType declType = st_resolve_type(type_nameBuf);

        check_variable_init(stmt, scope, declType, varName, isArray, arraySize, errors);
        break;
    }

    case ND_BLOCK: {
        // Each block gets its own scope so that inner declarations shadow
        // outer ones but do not persist beyond the closing brace.
        Scope *blockScope = sym_scopeCreate(scope);
        for (int i = 0; i < stmt->nchildren; i++)
            check_stmt(stmt->children[i], blockScope, returnType, arena, errors);
        // blockScope remains alive as a child of scope; freed by
        // sym_finalize() at the end of compilation.
        break;
    }

    case ND_IF:
        check_expr_type(stmt->children[0], scope, errors);                         // condition
        check_stmt(stmt->children[1], scope, returnType, arena, errors);      // then-branch
        if (stmt->nchildren > 2)
            check_stmt(stmt->children[2], scope, returnType, arena, errors);  // else-branch
        break;

    case ND_WHILE:
        check_expr_type(stmt->children[0], scope, errors);                     // loop condition
        check_stmt(stmt->children[1], scope, returnType, arena, errors);  // loop body
        break;

    case ND_RETURN: {
        DataType t = check_expr_type(stmt->children[0], scope, errors);
        if (t != T_VOID && !is_type_compatible(returnType, t)) {
            report_error(errors, "Errore: return di tipo %s non compatibile col tipo %s della funzione\n",
                        type_name(t), type_name(returnType));
        }
        break;
    }

    /* ---- Expression statement: call(), assignment as statement, etc. ----- */
    case ND_EXPR_STMT:
        check_expr_type(stmt->children[0], scope, errors);
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
 * all parameters in it, then hands off the body block to check_stmt().
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
static void check_function_body(ASTNode *decl, Scope *global,
                               Arena *arena, int *errors) {
    // Extract return type and function name from the declaration text
    // (format: "retType funcName", e.g. "int main" or "float compute").
    char *type_nameBuf, *funcName;
    int isArray, arraySize;
    st_elaborate_decl(arena, decl->text, &type_nameBuf, &funcName,
                            &isArray, &arraySize);

    DataType returnType = st_resolve_type(type_nameBuf);

    // Parameter scope is a direct child of global so forward references to
    // other top-level functions are visible from inside the body.
    Scope *fnScope = sym_scopeCreate(global);

    // All children except the last are parameters (ND_PARAM nodes).
    // SysV x86-64: at most 6 integer and 8 float args in registers; the
    // backend has no stack-argument path, so reject earlier here.
    int paramCount = decl->nchildren - 1;
    int nIntParams = 0, nFloatParams = 0;
    for (int p = 0; p < paramCount; p++) {
        char *ptype_name, *pname;
        int pIsArray, pArraySize;
        st_elaborate_decl(arena, decl->children[p]->text,
                          &ptype_name, &pname, &pIsArray, &pArraySize);
        if (st_resolve_type(ptype_name) == T_FLOAT)
            nFloatParams++;
        else
            nIntParams++;

        if (!st_bind_symbol(arena, fnScope, decl->children[p]))
            (*errors)++;
    }
    if (nIntParams > 6) {
        report_error(errors,
            "Errore: '%s' ha %d parametri interi, massimo supportato 6\n",
            funcName, nIntParams);
    }
    if (nFloatParams > 8) {
        report_error(errors,
            "Errore: '%s' ha %d parametri float, massimo supportato 8\n",
            funcName, nFloatParams);
    }

    // The last child is always the body block.
    ASTNode *body = decl->children[decl->nchildren - 1];
    check_stmt(body, fnScope, returnType, arena, errors);
}


int semantic_check(ASTNode *program, Scope *global) {
    int errors = 0;

    // One arena for all scratch allocations during the entire walk.
    // Destroyed at the end: no per-allocation free needed.
    Arena *arena = arena_create(0);

    const int nchildren = program->nchildren;
    ASTNode **children  = program->children;

    for (int i = 0; i < nchildren; i++) {
        ASTNode *decl = children[i];
        
        // Global variable declarations were fully handled by Pass 1
        // (st_resolve_global_namespace); only function bodies need walking here.
        if (decl->kind == ND_FUNC_DECL) {
            check_function_body(decl, global, arena, &errors);
        }
    }

    arena_destroy(arena);
    return errors;
}