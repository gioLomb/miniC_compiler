#ifndef SYMBOL_TABLE_H
#define SYMBOL_TABLE_H

/**
 * @file symbol_table.h
 * @brief Lexical Scope Symbol Table implementation ("sheaf of tables").
 *
 * Implements lexical scoping where each scope maintains its own Hash_Table and
 * forms a tree hierarchy using parent and children links.
 */

#include <stdint.h>
#include "hash_table.h"

/**
 * @brief Suggested maximum length buffer for manual string operations.
 * @note Hash table key allocation accepts arbitrarily long symbol identifiers.
 */
#define SYM_MAX_NAME_LEN   64

/**
 * @brief Maximum supported function parameters (32-bit bitfield / 2 bits per param).
 */
#define SYM_MAX_PARAMS     16

/**
 * @brief Default initial capacity for local scope hash tables.
 */
#define SCOPE_DEFAULT_CAPACITY 17

/**
 * @brief Enumeration of primitive source code data types.
 */
typedef enum {
    T_INT,    /**< Integer type */
    T_FLOAT,  /**< Floating-point type */
    T_VOID    /**< Void/empty type */
} DataType;

/**
 * @brief Enumeration of symbol entity classifications.
 */
typedef enum {
    SYM_VAR,  /**< Variable or array identifier */
    SYM_FUNC  /**< Function identifier */
} SymbolKind;

/**
 * @brief Plain Old Data (POD) structure storing symbol metadata.
 *
 * Holds variable/function attributes, resolution coordinates, and parameter lists.
 */
typedef struct {
    SymbolKind kind;        /**< Entity type (variable vs function) */
    DataType   dataType;    /**< Variable data type or function return type */
    int        isArray;     /**< Flag indicating if symbol is an array (1) or scalar (0) */
    int        arraySize;   /**< Capacity of array if `isArray` is set */

    /* Resolution coordinates for lexical shadowing */
    int        scopeLevel;  /**< Lexical depth level where declared */
    int        offset;      /**< Index offset in local scope hash table */

    /* Function specific metadata (valid if kind == SYM_FUNC) */
    int        paramCount;  /**< Number of function parameters */
    uint32_t   paramTypes;  /**< Bit-packed parameter types (2 bits per param) */

    /* Source code location coordinates for diagnostics */
    int line;               /**< Source line coordinate */
    int col;                /**< Source column coordinate */
} Symbol;

/**
 * @brief Packs a parameter's DataType into a bitmask integer.
 *
 * @param paramTypes Pointer to the destination bitmask variable.
 * @param index      Zero-based parameter index (0 to SYM_MAX_PARAMS - 1).
 * @param type       DataType enum to pack.
 */
static inline void symtab_pack_param_type(uint32_t *paramTypes, int index, DataType type) {
    uint32_t shift = (uint32_t)(index * 2);
    *paramTypes &= ~(0x3u << shift);          // Clear existing 2-bit field
    *paramTypes |= ((uint32_t)type & 0x3u) << shift;
}

/**
 * @brief Unpacks a parameter's DataType from a bitmask integer.
 *
 * @param paramTypes Bitmask variable containing packed types.
 * @param index      Zero-based parameter index to read.
 * @return Extracted DataType value.
 */
static inline DataType symtab_unpack_param_type(uint32_t paramTypes, int index) {
    uint32_t shift = (uint32_t)(index * 2);
    return (DataType)((paramTypes >> shift) & 0x3u);
}

/**
 * @brief Structure representing a lexical scope node in the scope tree.
 */
typedef struct Scope {
    Hash_Table *table;       /**< Local hash table mapping names to Symbol entries */
    struct Scope *parent;    /**< Pointer to enclosing outer scope (NULL for global root) */
    struct Scope **children; /**< Dynamic array of pointers to nested sub-scopes */
    int childCount;          /**< Number of active child scopes */
    int childCap;            /**< Allocated capacity for child scope pointer array */
    int level;               /**< Lexical nesting depth level (0 = global) */
} Scope;

/**
 * @brief Creates a new scope attached to a parent scope.
 *
 * @param parent Pointer to parent scope (NULL creates global root scope).
 * @return Pointer to newly initialized Scope instance, or NULL on memory error.
 */
Scope *sym_scopeCreate(Scope *parent);

/**
 * @brief Exits current scope context by returning its parent scope pointer.
 *
 * @param scope Pointer to current active Scope.
 * @return Pointer to parent Scope, or NULL if at root scope.
 */
Scope *sym_scopeExit(Scope *scope);

/**
 * @brief Declares a new symbol inside the current scope ONLY.
 *
 * @param scope Pointer to target local Scope.
 * @param name  Symbol identifier string.
 * @param sym   Pointer to Symbol descriptor to copy into table.
 * @return 1 on successful declaration, 0 if symbol exists in local scope or on invalid input.
 */
int sym_bind(Scope *scope, const char *name, const Symbol *sym);

/**
 * @brief Resolves a symbol name by ascending through outer enclosing scopes.
 *
 * @param scope Pointer to starting inner Scope.
 * @param name  Identifier string to lookup.
 * @param out   Pointer receiving copy of matched Symbol metadata.
 * @return 1 if symbol was found in scope chain, 0 otherwise.
 */
int sym_resolve(Scope *scope, const char *name, Symbol *out);

/**
 * @brief Recursively destroys a scope tree and frees all associated memory.
 *
 * @param root Pointer to root Scope of subtree to destroy.
 */
void sym_finalize(Scope *root);

#endif /* SYMBOL_TABLE_H */
