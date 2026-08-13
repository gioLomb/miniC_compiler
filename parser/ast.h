#ifndef AST_H
#define AST_H

#include "../arena.h"

/*
 * X-Macro list: unico punto di verita' per NodeKind.
 * Ogni entry: X(enum_value, "stringa_leggibile")
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
typedef enum {
    NODEKIND_LIST(X_ENUM)
} NodeKind;
#undef X_ENUM

typedef struct ASTNode {
    NodeKind kind;
    char *text;          /* punta dentro astArena — non va liberato */
    struct ASTNode **children;
    int nchildren;
    int capacity;

    /* coordinate di risoluzione (popolate da semantic_check) */
    int scopeLevel;
    int offset;
} ASTNode;

/*
 * Crea un nodo: text viene duplicato nell'arena fornita.
 * Passare arena=NULL è equivalente a non avere testo (text resterà NULL).
 */
ASTNode *newNode(Arena *arena, NodeKind kind, const char *text);

void addChild(ASTNode *parent, ASTNode *child);
void printAST(const ASTNode *node, int depth);

/*
 * Libera la struttura ad albero (children array + nodi stessi).
 * NON libera node->text: la memoria appartiene all'astArena del chiamante.
 */
void freeAST(ASTNode *node);

#endif