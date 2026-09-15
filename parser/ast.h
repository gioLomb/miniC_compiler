#ifndef AST_H
#define AST_H


/**
 * @file ast.h
 * @brief Abstract Syntax Tree (AST) representation and manipulation interface.
 *
 * Defines the node kinds, the recursive AST data structures and the
 * helper functions used to build, traverse and pretty-print the tree
 * produced by the parser. Serves as the primary intermediate form
 * between parsing and semantic analysis / IR generation.
 */

#include "../arena.h"
#include "../symbol_table.h"   /* for DataType */

/**
 * @brief X-Macro list defining AST node kinds and their string representations.
 *
 * Single source of truth for AST node types.
 * Each entry format: X(enum_value, "readable_string")
 */
#define NODEKIND_LIST(X)                          \
    X(ND_PROGRAM,      "Program"     )            \
    X(ND_BLOCK,        "Block"       )            \
    X(ND_VAR_DECL,     "VarDecl"    )            \
    X(ND_FUNC_DECL,    "FuncDecl"   )            \
    X(ND_PARAM,        "Param"      )            \
    X(ND_IF,           "If"         )            \
    X(ND_WHILE,        "While"      )            \
    X(ND_RETURN,       "Return"     )            \
    X(ND_EXPR_STMT,    "ExprStmt"   )            \
    X(ND_ASSIGN,       "Assign"     )            \
    X(ND_BINOP,        "BinOp"      )            \
    X(ND_UNARY,        "UnaryOp"    )            \
    X(ND_CALL,         "Call"       )            \
    X(ND_ARRAY_ACCESS, "ArrayAccess")            \
    X(ND_ID,           "Id"         )            \
    X(ND_NUM_INT,      "NumInt"     )            \
    X(ND_NUM_FLOAT,    "NumFloat"   )            \
    X(ND_ERROR,        "Error"      )

#define X_ENUM(val, str) val,
/**
 * @brief Enumeration of supported AST node types.
 */
typedef enum {
    NODEKIND_LIST(X_ENUM)
} NodeKind;
#undef X_ENUM

/**
 * @brief Structure representing a node in the Abstract Syntax Tree.
 */
typedef struct ASTNode {
    NodeKind kind;              /**< Specific node kind classification */
    char *text;                 /**< Text payload allocated within arena (do NOT free directly) */
    struct ASTNode **children;  /**< Dynamic array of pointers to child nodes */
    int nchildren;              /**< Current number of child nodes attached */
    int capacity;               /**< Allocated capacity for the children array */

    /* Resolution coordinates (populated during semantic analysis pass) */
    int scopeLevel;             /**< Nesting scope level where symbol resolves */
    int offset;                 /**< Memory offset inside frame or storage area */
    DataType dataType;          /**< Resolved type of this expression/decl node,
                                  *   stamped during semantic analysis; drives
                                  *   int-vs-float codegen in instr_selector.c. */
} ASTNode;

/**
 * @brief Constructs a new AST node.
 *
 * The node struct itself is allocated from @p arena (bump allocation,
 * never individually freed) and @p text is duplicated into the same
 * arena. Passing `text = NULL` leaves the `text` field NULL.
 *
 * @param arena Memory arena that owns both the node and its text; must not be NULL.
 * @param kind  Node classification.
 * @param text  Lexeme or string literal associated with the node.
 * @return Pointer to the newly allocated ASTNode.
 */
ASTNode *newNode(Arena *arena, NodeKind kind, const char *text);

/**
 * @brief Appends a child node to a parent's dynamic children array.
 *
 * Grows the children buffer from @p arena when full, doubling capacity.
 * Arena has no per-allocation free/realloc: on growth a fresh, larger
 * block is allocated and the live entries copied over; the previous
 * (smaller) block is simply abandoned inside the arena rather than freed
 * (bounded, amortised waste — same pattern used for Hash_Table value
 * buffers in hash_table.c).
 *
 * @param arena  Arena that owns @p parent's children buffer (the same
 *               arena the tree is being built from).
 * @param parent Pointer to the parent node.
 * @param child  Pointer to the child node to append.
 */
void addChild(Arena *arena, ASTNode *parent, ASTNode *child);

/**
 * @brief Recursively prints formatted AST tree hierarchy to stdout.
 *
 * @param node  Pointer to root or current sub-tree node.
 * @param depth Current recursion/indentation depth level.
 */
void printAST(const ASTNode *node, int depth);

#endif /* AST_H */