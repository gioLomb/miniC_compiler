#ifndef AST_H
#define AST_H

/*
 * X-Macro list: unico punto di verita' per NodeKind.
 * Ogni entry: X(enum_value, "stringa_leggibile")
 *
 * Aggiungere/rimuovere un nodo: modifica SOLO questa lista.
 * enum e array di stringhe (in ast.c) si aggiornano automaticamente
 * alla prossima compilazione — zero rischio di desync.
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

/* Espansione dell'enum: ogni X(val, str) diventa 'val,' */
#define X_ENUM(val, str) val,
typedef enum {
    NODEKIND_LIST(X_ENUM)
} NodeKind;
#undef X_ENUM

typedef struct ASTNode {
    NodeKind kind;
    char *text;
    struct ASTNode **children;
    int nchildren;
    int capacity;

    /* Coordinate di risoluzione, popolate da semantic_check su ND_ID,
       ND_ARRAY_ACCESS, ND_VAR_DECL e ND_PARAM (-1 altrove / non ancora
       risolto). Identificano univocamente la variabile (shadowing incluso)
       senza che ir_generate debba ripetere una symtab_lookup: scopeLevel e'
       la profondita' lessicale dello scope in cui la variabile e' stata
       dichiarata, offset la sua posizione nella tabella locale di quello
       scope. */
    int scopeLevel;
    int offset;
} ASTNode;

ASTNode *newNode(NodeKind kind, const char *text);
void     addChild(ASTNode *parent, ASTNode *child);
void     printAST(const ASTNode *node, int depth);
void     freeAST(ASTNode *node);

#endif