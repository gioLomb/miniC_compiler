#ifndef AST_H
#define AST_H

#include "../arena.h"

/**
 * @file ast.h
 * @brief Abstract Syntax Tree (AST) representation and manipulation interface.
 */

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
} ASTNode;

/**
 * @brief Constructs a new AST node.
 *
 * Allocates space for the node and duplicates the text into the provided arena.
 * Passing `arena = NULL` or `text = NULL` leaves `text` field initialized to `NULL`.
 *
 * @param arena Pointer to the memory arena used to store `text`.
 * @param kind  Node classification.
 * @param text  Lexeme or string literal associated with the node.
 * @return Pointer to the newly allocated ASTNode.
 */
ASTNode *newNode(Arena *arena, NodeKind kind, const char *text);

/**
 * @brief Appends a child node to a parent's dynamic children array.
 *
 * Automatically resizes the parent node's children buffer if full.
 *
 * @param parent Pointer to the parent node.
 * @param child  Pointer to the child node to append.
 */
void addChild(ASTNode *parent, ASTNode *child);

/**
 * @brief Recursively prints formatted AST tree hierarchy to stdout.
 *
 * @param node  Pointer to root or current sub-tree node.
 * @param depth Current recursion/indentation depth level.
 */
void printAST(const ASTNode *node, int depth);

/**
 * @brief Recursively frees dynamically allocated AST structures.
 *
 * Frees parent and child node structures and their dynamic `children` array buffers.
 * @note Does NOT free `node->text` memory as it belongs to the caller's arena.
 *
 * @param node Pointer to the root AST node to deallocate.
 */
void freeAST(ASTNode *node);

#endif /* AST_H */