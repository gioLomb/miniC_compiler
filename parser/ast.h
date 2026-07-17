#ifndef AST_H
#define AST_H

/* Tipi di nodo dell'albero sintattico (parse tree / AST) */
typedef enum {
    ND_PROGRAM,       /* radice: lista di dichiarazioni/istruzioni top-level  */
    ND_BLOCK,         /* '{' Stmt* '}'                                       */
    ND_VAR_DECL,      /* dichiarazione di variabile o array; text = "tipo
                         nome" o "tipo nome[size]"; figli (opzionali) =
                         inizializzatore: 1 figlio se scalare, N figli
                         (uno per elemento tra { }) se array           */
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

    /* Coordinate di risoluzione, popolate da semantic_check su ND_ID,
       ND_ARRAY_ACCESS, ND_VAR_DECL e ND_PARAM (-1 altrove / non ancora
       risolto). Identificano univocamente la variabile (shadowing incluso)
       senza che ir_generate debba ripetere una symtab_lookup: scopeLevel e'
       la profondita' lessicale dello scope in cui la variabile e' stata
       dichiarata, offset la sua posizione nella tabella locale di quello
       scope. Vedi report_shadowing_ir.md per il ragionamento completo. */
    int scopeLevel;
    int offset;
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