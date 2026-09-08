/**
 * @file ast_to_symtab.h
 * @brief AST → Symbol Table translation interface.
 *
 * Bridges the parser output (ASTNode tree) and the symbol-table layer
 * (Scope / Symbol).  Three distinct responsibilities are exposed:
 *
 *  1. **Declaration text parsing** (st_elaborate_decl)
 *     ASTNode.text for declarations is a compact string produced by the
 *     parser (e.g. "int x", "float arr[5]").  st_elaborate_decl splits it
 *     into its constituent parts — type name, symbol name, array flag and
 *     size — allocating the substrings from the caller's Arena.
 *
 *  2. **Single-symbol binding** (st_bind_symbol)
 *     Parses a single ND_VAR_DECL or ND_PARAM node's text and inserts the
 *     resulting Symbol into the given Scope.  On success it also stamps the
 *     AST node with the resolved (scopeLevel, offset) coordinates so that
 *     ir_generate() can identify the variable without a second lookup.
 *
 *  3. **Pass 1 — global namespace population** (st_resolve_global_namespace)
 *     Walks the top-level children of ND_PROGRAM and registers every global
 *     variable and function signature in the global Scope.  This single pass
 *     ensures that forward references between functions (function A calls
 *     function B declared later in the file) resolve correctly: all
 *     signatures are visible before any function body is type-checked or
 *     translated to IR.
 *
 * Dependency: caller must create the global Scope (sym_scopeCreate(NULL))
 * before calling st_resolve_global_namespace, and must call sym_finalize()
 * when done to release all scope memory.
 */

#ifndef AST_TO_SYMTAB_H
#define AST_TO_SYMTAB_H

#include <stddef.h>
#include "arena.h"
#include "parser/ast.h"
#include "symbol_table.h"


/**
 * @brief Map a textual type name from the AST to its DataType enum value.
 *
 * Recognised strings: "int" → T_INT, "float" → T_FLOAT.
 * Any other string (including NULL) falls back to T_VOID.
 *
 * Only the first character is examined for speed — the parser guarantees
 * that only valid type keywords reach here.
 *
 * @param type_name  Null-terminated type keyword string (e.g. "int", "float").
 * @return          Corresponding DataType value; T_VOID on unrecognised input.
 */
DataType st_resolve_type(const char *type_name);

/* =========================================================================
 * Declaration text parsing
 * ========================================================================= */

/**
 * @brief Decompose a parser-generated declaration string into its parts.
 *
 * The parser encodes declarations as a single compact string in ASTNode.text:
 *
 *   Scalar variable: "int x"           → type="int",   name="x",   isArray=0
 *   Array variable:  "float arr[10]"   → type="float", name="arr", isArray=1, arraySize=10
 *   Function/param:  "int main"        → type="int",   name="main",isArray=0
 *
 * All output strings are duplicated into @p arena so they share the AST's
 * lifetime and need no individual free.
 *
 * @param arena       Arena used to allocate the output substrings.
 * @param text        Raw declaration string from ASTNode.text.
 * @param outTypeName Receives a pointer to the type name substring.
 * @param outName     Receives a pointer to the identifier name substring.
 * @param isArray     Set to 1 if the declaration contains "[size]", else 0.
 * @param arraySize   Set to the parsed element count when isArray is 1.
 */
void st_elaborate_decl(Arena *arena, const char *text,
                       char **outTypeName, char **outName,
                       int *isArray, int *arraySize);

/* =========================================================================
 * Single-symbol binding
 * ========================================================================= */

/**
 * @brief Parse a declaration node's text and insert the symbol into @p scope.
 *
 * Calls st_elaborate_decl() to split the node's text, builds a Symbol
 * descriptor, and delegates to sym_bind().  On success the AST node is
 * annotated with the resolved coordinates:
 *   - node->scopeLevel = scope->level
 *   - node->offset     = the slot index assigned within @p scope
 *
 * These coordinates are consumed later by ir_generate() to identify each
 * variable unambiguously even under shadowing, without re-querying the
 * symbol table.
 *
 * @param arena  Scratch arena for temporary substring allocations.
 * @param scope  Target scope that receives the new symbol.
 * @param node   ND_VAR_DECL or ND_PARAM AST node whose text is parsed.
 * @return       1 on success; 0 if the name is already declared in @p scope
 *               (redeclaration error — caller should increment its error counter).
 */
int st_bind_symbol(Arena *arena, Scope *scope, ASTNode *node);


/**
 * @brief Populate @p global with every top-level declaration in @p program.
 *
 * Iterates over the direct children of the ND_PROGRAM root and registers:
 *   - ND_FUNC_DECL nodes → SYM_FUNC symbols with packed parameter types.
 *   - ND_VAR_DECL nodes  → SYM_VAR symbols (scalars and arrays).
 *
 * Each symbol receives a unique sequential offset (global->table->size
 * before the sym_bind call) so that OPND_VAR operands emitted by
 * ir_generate() can identify globals by (varLevel=0, varOffset=offset).
 * The offset counter advances for both vars and funcs to stay in sync with
 * the order in which st_resolve_global_namespace processes the declarations.
 *
 * Global ND_VAR_DECL nodes are stamped with (scopeLevel, offset) so that
 * ir_add_global() in ir.c can match them to the correct IRGlobalVar entry
 * without a second symbol-table lookup.
 *
 * Forward-reference correctness: because all signatures are recorded before
 * any function body is walked by semantic_check(), a call to a function
 * declared later in the file resolves without error.
 *
 * @param program  Root ND_PROGRAM node produced by ParseProgram().
 * @param global   Empty global Scope created by the caller.
 * @return         Number of redeclaration errors encountered (0 = success).
 */
int st_resolve_global_namespace(ASTNode *program, Scope *global);

#endif /* AST_TO_SYMTAB_H */