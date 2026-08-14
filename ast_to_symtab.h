#ifndef AST_TO_SYMTAB_H
#define AST_TO_SYMTAB_H

#include <stddef.h>
#include "arena.h"
#include "parser/ast.h"
#include "symbol_table.h"

/**
 * @file ast_to_symtab.h
 * @brief AST to Symbol Table translation interface.
 *
 * Provides utilities to parse AST declaration text nodes and populate symbol tables.
 */

/**
 * @brief Converts a textual type representation from the AST (e.g., "int", "float") to its DataType enum.
 *
 * Unrecognized type strings default to fallback value `T_VOID`.
 *
 * @param typeName Name string of the data type.
 * @return Corresponding DataType enum value.
 */
DataType st_resolveType(const char *typeName);

/**
 * @brief Parses a combined declaration string from an AST node's text field into individual components.
 *
 * Splits formatted strings (e.g., "int x", "int arr[5]", "int functionName") into type, identifier name,
 * and array metadata using memory allocated inside `arena`.
 *
 * @param arena       Pointer to the Arena allocator used for string memory.
 * @param text        The raw declaration string from `node->text`.
 * @param outTypeName Output pointer receiving the extracted type name string.
 * @param outName     Output pointer receiving the extracted symbol identifier.
 * @param isArray     Output flag set to 1 if declaration is an array, 0 otherwise.
 * @param arraySize   Output integer receiving array capacity if applicable.
 */
void st_elaborateDecl(Arena *arena, const char *text,
                             char **outTypeName, char **outName,
                             int *isArray, int *arraySize);

/**
 * @brief Declares a variable or parameter symbol in a target scope from a node's declaration text.
 *
 * Parses `node->text` and registers the symbol in `scope`. Stamps resolution coordinates
 * (`node->scopeLevel` and `node->offset`) directly onto the node for intermediate representation generation.
 *
 * @param arena Pointer to the memory arena used for temporary string parsing allocations.
 * @param scope Pointer to the target Scope structure.
 * @param node  Pointer to the AST node (`ND_VAR_DECL` or `ND_PARAM`).
 * @return 1 on successful declaration, 0 if a redeclaration error occurs in the current scope.
 */
int st_bindSymbol(Arena *arena, Scope *scope, ASTNode *node);

/**
 * @brief Populates the global scope with top-level AST declarations (Pass 1).
 *
 * Traverses top-level children of `ND_PROGRAM` and populates the `global` symbol table with
 * function signatures and global variables to resolve forward references.
 *
 * @param program Pointer to the root `ND_PROGRAM` AST node.
 * @param global  Pointer to the target global Scope structure.
 * @return Number of redeclaration errors encountered (0 indicates success).
 */
int st_resolveGlobalNamespace(ASTNode *program, Scope *global);

#endif /* AST_TO_SYMTAB_H */