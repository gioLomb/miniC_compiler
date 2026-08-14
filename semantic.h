#ifndef SEMANTIC_H
#define SEMANTIC_H

#include "parser/ast.h"
#include "symbol_table.h"

/**
 * @file semantic.h
 * @brief Single-pass semantic analysis: scope creation, name resolution,
 *        and type checking.
 *
 * Merges what used to be two separate phases:
 *   - **Pass 2**: creation of parameter/block scopes and declaration of
 *     local variables.
 *   - **Semantic verification**: name resolution and type checking of every
 *     use site (ND_ID, ND_CALL, ND_ARRAY_ACCESS, assignments, operators,
 *     return statements).
 *
 * Rationale for merging: the scopes opened during Pass 2 (one per function
 * body, one per nested block) were local to the C call-frame of the old
 * walking function and disappeared when it returned — no clean way for a
 * later phase to retrieve them for lookups.  By fusing the two steps, the
 * scope in which a variable is declared and the scope in which its uses
 * are resolved are the **same local variable in the same recursive call**:
 * nothing needs to be exported or re-queried.
 *
 * @pre  @p global must already be populated with all top-level signatures
 *       by symtab_populate_globals() (Pass 1, ast_to_symtab.h) before
 *       calling semantic_check().  Forward references between functions
 *       (a function calling another declared later in the file) would not
 *       resolve otherwise.
 *
 * ### Checks performed
 *
 * | Node kind          | What is verified |
 * |--------------------|-----------------|
 * | ND_ID              | Declared in scope; not an array; not a function. |
 * | ND_ARRAY_ACCESS    | Symbol is an array; index type is `int`; if index is a compile-time constant, bounds are checked (0 ≤ idx < arraySize). |
 * | ND_CALL            | Name refers to a function; arity matches `paramCount`; each argument is compatible with the corresponding parameter type. |
 * | ND_ASSIGN          | LHS must be ND_ID or ND_ARRAY_ACCESS; only implicit `int`→`float` widening is allowed; `float`→`int` narrowing is always an error. |
 * | ND_BINOP `%`       | Both operands must be `int`. |
 * | ND_RETURN          | Return-expression type must be compatible with the function's declared return type (same widening rule). |
 *
 * ### Scope lifetime
 * Scopes created here (one per function, one per nested ND_BLOCK) are
 * attached as children of @p global and remain alive for as long as
 * @p global does.  Destruction is handled externally via
 * symtab_destroy_tree(global) — semantic_check() never frees anything.
 *
 * ### Resolution coordinates
 * Every resolved ND_ID, ND_ARRAY_ACCESS, ND_VAR_DECL, and ND_PARAM node
 * has its @c scopeLevel and @c offset fields set to the values stored in
 * the matching Symbol.  This lets ir_generate() identify every variable
 * unambiguously (even in the presence of shadowing) without performing
 * another symbol-table lookup.
 *
 * @param program  Root ND_PROGRAM node produced by ParseProgram().
 * @param global   Global scope pre-populated by symtab_populate_globals().
 * @return         Total number of semantic errors (0 = no errors).
 */
int semantic_check(ASTNode *program, Scope *global);

#endif