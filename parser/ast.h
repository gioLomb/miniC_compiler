#ifndef AST_H
#define AST_H

/* Tipi di nodo dell'albero sintattico (parse tree / AST) */
typedef enum {
    ND_PROGRAM,       /* radice: lista di dichiarazioni/istruzioni top-level  */
    ND_BLOCK,         /* '{' Stmt* '}'                                       */
    ND_VAR_DECL,      /* dichiarazione di variabile o array                  */
    ND_FUNC_DECL,     /* dichiarazione/definizione di funzione               */
    ND_PARAM,         /* singolo parametro formale di una funzione           */
    ND_IF,            /* figli: [cond, then, (else)?]                       */
    ND_WHILE,         /* figli: [cond, body]                                */
    ND_RETURN,        /* figli: [expr]                                       */
    ND_EXPR_STMT,     /* figli: [expr]  (espressione usata come istruzione)  */
    ND_ASSIGN,        /* figli: [lvalue, rvalue]                             */
    ND_BINOP,         /* figli: [sx, dx]; text = operatore ("+", "&&", ...)  */
    ND_UNARY,         /* figli: [operando]; text = operatore ("!")           */
    ND_CALL,          /* text = nome funzione (nessun figlio: no argomenti)  */
    ND_ARRAY_ACCESS,  /* figli: [indice]; text = nome array                  */
    ND_ID,            /* foglia: text = nome identificatore                  */
    ND_NUM_INT,       /* foglia: text = valore letterale intero              */
    ND_NUM_FLOAT,      /* foglia: text = valore letterale float               */
    ND_ERROR
} NodeKind;

typedef struct ASTNode {
    NodeKind kind;
    char *text;                 /* lessema associato al nodo (puo' essere NULL) */
    struct ASTNode **children;
    int nchildren;
    int capacity;
} ASTNode;

/* Crea un nuovo nodo; 'text' viene copiato internamente (puo' essere NULL). */
ASTNode *newNode(NodeKind kind, const char *text);

/* Aggiunge 'child' come figlio di 'parent' (child puo' essere NULL: viene ignorato). */
void addChild(ASTNode *parent, ASTNode *child);

/* Stampa l'albero in forma indentata su stdout. */
void printAST(const ASTNode *node, int depth);

/* Libera ricorsivamente tutto l'albero. */
void freeAST(ASTNode *node);

#endif