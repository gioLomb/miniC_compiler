#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../tokens.h" // Qui devi definire i tuoi TOK_*
#include "ast.h"

// Prototipi dello scanner (generato da re2c)
extern const unsigned char *cur;
extern const unsigned char *tok;
extern int yylex();

int current_token;
char *current_lexeme = NULL;  // testo dell'ultimo token letto (per costruire l'AST)

// Prototipi del parser: ogni Parse* costruisce e restituisce un nodo dell'AST
ASTNode *ParseProgram();
ASTNode *ParseBlock();
ASTNode *ParseStmt();
ASTNode *ParseDeclaration();
void     ParseParamList(ASTNode *funcNode);
ASTNode *ParseExpr();
ASTNode *ParseAssign();
ASTNode *ParseLogicOr();
ASTNode *ParseLogicAnd();
ASTNode *ParseEquality();
ASTNode *ParseRelational();
ASTNode *ParseAdditive();
ASTNode *ParseTerm();
ASTNode *ParseUnary();
ASTNode *ParseFactor();
void     match(int expected);

// Copia in current_lexeme il testo [tok, cur) dell'ultimo token letto da yylex().
// Va chiamata SUBITO dopo yylex(), prima che una chiamata successiva sovrascriva tok/cur.
static void setLexeme(void) {
    free(current_lexeme);
    int len = (int)(cur - tok);
    if (len < 0) len = 0;
    current_lexeme = (char *)malloc(len + 1);
    memcpy(current_lexeme, tok, len);
    current_lexeme[len] = '\0';
}

// Avanza al prossimo token, aggiornando sia current_token che current_lexeme.
static void advance(void) {
    current_token = yylex();
    setLexeme();
}

void match(int expected) {
    if (current_token == expected) {
        advance();
    } else {
        fprintf(stderr, "Errore di sintassi: atteso token %d, trovato %d\n", expected, current_token);
        exit(1);
    }
}

// Program -> Stmt*
ASTNode *ParseProgram() {
    ASTNode *node = newNode(ND_PROGRAM, NULL);
    while (current_token != TOK_EOF) {
        addChild(node, ParseStmt());
    }
    return node;
}

// Block -> '{' Stmt* '}'
ASTNode *ParseBlock() {
    match(TOK_DEL_LBRACE);
    ASTNode *node = newNode(ND_BLOCK, NULL);
    while (current_token != TOK_DEL_RBRACE) {
        addChild(node, ParseStmt());
    }
    match(TOK_DEL_RBRACE);
    return node;
}

// Stmt -> Decl | Block | IfStmt | WhileStmt | ReturnStmt | Expr ';'
ASTNode *ParseStmt() {
    switch (current_token) {
        case TOK_KW_INT:
        case TOK_KW_FLOAT:
            return ParseDeclaration();

        case TOK_DEL_LBRACE:
            return ParseBlock();

        case TOK_KW_IF: {
            match(TOK_KW_IF);
            match(TOK_DEL_LPAREN);
            ASTNode *cond = ParseExpr();
            match(TOK_DEL_RPAREN);
            ASTNode *thenBranch = ParseStmt();

            ASTNode *node = newNode(ND_IF, NULL);
            addChild(node, cond);
            addChild(node, thenBranch);
            if (current_token == TOK_KW_ELSE) {
                match(TOK_KW_ELSE);
                addChild(node, ParseStmt()); // terzo figlio = ramo else, se presente
            }
            return node;
        }

        case TOK_KW_WHILE: {
            match(TOK_KW_WHILE);
            match(TOK_DEL_LPAREN);
            ASTNode *cond = ParseExpr();
            match(TOK_DEL_RPAREN);
            ASTNode *body = ParseStmt();

            ASTNode *node = newNode(ND_WHILE, NULL);
            addChild(node, cond);
            addChild(node, body);
            return node;
        }

        case TOK_KW_RETURN: {
            match(TOK_KW_RETURN);
            ASTNode *expr = ParseExpr();
            match(TOK_DEL_SEMICOLON);

            ASTNode *node = newNode(ND_RETURN, NULL);
            addChild(node, expr);
            return node;
        }

        default: {
            ASTNode *expr = ParseExpr();
            match(TOK_DEL_SEMICOLON);

            ASTNode *node = newNode(ND_EXPR_STMT, NULL);
            addChild(node, expr);
            return node;
        }
    }
}

// ParamList -> (Type ID (',' Type ID)*)?
// I parametri vengono aggiunti direttamente come figli del nodo funcNode.
void ParseParamList(ASTNode *funcNode) {
    while (current_token == TOK_KW_INT || current_token == TOK_KW_FLOAT) {
        char type[32];
        snprintf(type, sizeof(type), "%s", current_lexeme);
        match(current_token);

        char name[64];
        snprintf(name, sizeof(name), "%s", current_lexeme);
        match(TOK_ID);

        char combined[100];
        snprintf(combined, sizeof(combined), "%s %s", type, name);
        addChild(funcNode, newNode(ND_PARAM, combined));

        if (current_token == TOK_DEL_COMMA) {
            match(TOK_DEL_COMMA);
        } else {
            break;
        }
    }
}

// Decl -> Type ID '(' ParamList ')' Block            (definizione di funzione)
//       | Type ID ( '[' NUM ']' )? ';'                (variabile / array)
ASTNode *ParseDeclaration() {
    char type[32];
    snprintf(type, sizeof(type), "%s", current_lexeme);
    match(current_token); // Consuma KW_INT o KW_FLOAT

    char name[64];
    snprintf(name, sizeof(name), "%s", current_lexeme);
    match(TOK_ID);

    char combined[100];
    snprintf(combined, sizeof(combined), "%s %s", type, name);

    if (current_token == TOK_DEL_LPAREN) {
        // Definizione di funzione
        ASTNode *node = newNode(ND_FUNC_DECL, combined);
        match(TOK_DEL_LPAREN);
        ParseParamList(node);
        match(TOK_DEL_RPAREN);
        addChild(node, ParseBlock()); // corpo della funzione (ultimo figlio)
        return node;
    }

    if (current_token == TOK_DEL_LBRACK) {
        // Dichiarazione di array
        match(TOK_DEL_LBRACK);
        char size[32];
        snprintf(size, sizeof(size), "%s", current_lexeme);
        match(TOK_NUM_INT);
        match(TOK_DEL_RBRACK);

        char arrDecl[140];
        snprintf(arrDecl, sizeof(arrDecl), "%s[%s]", combined, size);
        match(TOK_DEL_SEMICOLON);
        return newNode(ND_VAR_DECL, arrDecl);
    }

    // Dichiarazione di variabile semplice
    match(TOK_DEL_SEMICOLON);
    return newNode(ND_VAR_DECL, combined);
}

// Gerarchia Espressioni (precedenza crescente):
// Expr -> Assign
// Assign -> LogicOr ( '=' Assign )?           (associativa a destra)
// LogicOr -> LogicAnd ( '||' LogicAnd )*
// LogicAnd -> Equality ( '&&' Equality )*
// Equality -> Relational ( ('=='|'!=') Relational )*
// Relational -> Additive ( ('<'|'>'|'<='|'>=') Additive )*
// Additive -> Term ( ('+'|'-') Term )*
// Term -> Unary ( ('*'|'/'|'%') Unary )*
// Unary -> '!' Unary | Factor
// Factor -> ID | NUM | '(' Expr ')' | Call | ArrayAccess

ASTNode *ParseExpr() {
    return ParseAssign();
}

ASTNode *ParseAssign() {
    ASTNode *left = ParseLogicOr();
    if (current_token == TOK_OP_ASSIGN) {
        match(TOK_OP_ASSIGN);
        ASTNode *right = ParseAssign(); // ricorsione per l'associativita' a destra
        ASTNode *node = newNode(ND_ASSIGN, "=");
        addChild(node, left);
        addChild(node, right);
        return node;
    }
    return left;
}

ASTNode *ParseLogicOr() {
    ASTNode *left = ParseLogicAnd();
    while (current_token == TOK_OP_OR) {
        char *op = strdup(current_lexeme);
        match(current_token);
        ASTNode *right = ParseLogicAnd();
        ASTNode *node = newNode(ND_BINOP, op);
        addChild(node, left);
        addChild(node, right);
        free(op);
        left = node;
    }
    return left;
}

ASTNode *ParseLogicAnd() {
    ASTNode *left = ParseEquality();
    while (current_token == TOK_OP_AND) {
        char *op = strdup(current_lexeme);
        match(current_token);
        ASTNode *right = ParseEquality();
        ASTNode *node = newNode(ND_BINOP, op);
        addChild(node, left);
        addChild(node, right);
        free(op);
        left = node;
    }
    return left;
}

ASTNode *ParseEquality() {
    ASTNode *left = ParseRelational();
    while (current_token == TOK_OP_EQ || current_token == TOK_OP_NE) {
        char *op = strdup(current_lexeme);
        match(current_token);
        ASTNode *right = ParseRelational();
        ASTNode *node = newNode(ND_BINOP, op);
        addChild(node, left);
        addChild(node, right);
        free(op);
        left = node;
    }
    return left;
}

ASTNode *ParseRelational() {
    ASTNode *left = ParseAdditive();
    while (current_token == TOK_OP_LT || current_token == TOK_OP_GT ||
           current_token == TOK_OP_LE || current_token == TOK_OP_GE) {
        char *op = strdup(current_lexeme);
        match(current_token);
        ASTNode *right = ParseAdditive();
        ASTNode *node = newNode(ND_BINOP, op);
        addChild(node, left);
        addChild(node, right);
        free(op);
        left = node;
    }
    return left;
}

ASTNode *ParseAdditive() {
    ASTNode *left = ParseTerm();
    while (current_token == TOK_OP_PLUS || current_token == TOK_OP_MINUS) {
        char *op = strdup(current_lexeme);
        match(current_token);
        ASTNode *right = ParseTerm();
        ASTNode *node = newNode(ND_BINOP, op);
        addChild(node, left);
        addChild(node, right);
        free(op);
        left = node;
    }
    return left;
}

ASTNode *ParseTerm() {
    ASTNode *left = ParseUnary();
    while (current_token == TOK_OP_MUL || current_token == TOK_OP_DIV || current_token == TOK_OP_MOD) {
        char *op = strdup(current_lexeme);
        match(current_token);
        ASTNode *right = ParseUnary();
        ASTNode *node = newNode(ND_BINOP, op);
        addChild(node, left);
        addChild(node, right);
        free(op);
        left = node;
    }
    return left;
}

// Unary -> '!' Unary | Factor
ASTNode *ParseUnary() {
    if (current_token == TOK_OP_NOT) {
        char *op = strdup(current_lexeme);
        match(TOK_OP_NOT);
        ASTNode *operand = ParseUnary();
        ASTNode *node = newNode(ND_UNARY, op);
        addChild(node, operand);
        free(op);
        return node;
    }
    return ParseFactor();
}

// Factor -> ID | NUM | '(' Expr ')' | Call | ArrayAccess
ASTNode *ParseFactor() {
    if (current_token == TOK_ID) {
        char *name = strdup(current_lexeme);
        match(TOK_ID);

        ASTNode *node;
        if (current_token == TOK_DEL_LPAREN) { // Chiamata funzione
            match(TOK_DEL_LPAREN);
            /* ParseArgs(); */ // TODO: argomenti non ancora supportati dalla grammatica
            match(TOK_DEL_RPAREN);
            node = newNode(ND_CALL, name);
        } else if (current_token == TOK_DEL_LBRACK) { // Accesso array
            match(TOK_DEL_LBRACK);
            ASTNode *index = ParseExpr();
            match(TOK_DEL_RBRACK);
            node = newNode(ND_ARRAY_ACCESS, name);
            addChild(node, index);
        } else {
            node = newNode(ND_ID, name);
        }
        free(name);
        return node;

    } else if (current_token == TOK_NUM_INT) {
        ASTNode *node = newNode(ND_NUM_INT, current_lexeme);
        match(TOK_NUM_INT);
        return node;

    } else if (current_token == TOK_NUM_FLOAT) {
        ASTNode *node = newNode(ND_NUM_FLOAT, current_lexeme);
        match(TOK_NUM_FLOAT);
        return node;

    } else if (current_token == TOK_DEL_LPAREN) {
        match(TOK_DEL_LPAREN);
        ASTNode *node = ParseExpr();
        match(TOK_DEL_RPAREN);
        return node;
    }

    fprintf(stderr, "Errore di sintassi: token inatteso %d in un'espressione\n", current_token);
    exit(1);
}

static unsigned char *readFile(const char *path, long *outLen) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Impossibile aprire il file %s\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    /* +1 byte per il terminatore '\0' usato come sentinella di fine-input
       (regola "end" nel blocco re2c sopra) */
    unsigned char *buf = malloc(len + 1);
    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);

    if (outLen) *outLen = len;
    return buf;
}


int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <file_sorgente.c>\n", argv[0]);
        return 1;
    }

    // Carica il file in memoria
    unsigned char *buffer = readFile(argv[1], NULL);
    cur = buffer; // Inizializza il cursore dello scanner

    // Avvia il parsing
    advance();
    ASTNode *root = ParseProgram();

    printf("Parsing completato con successo!\n\n");
    printf("=== PARSE TREE ===\n");
    printAST(root, 0);

    freeAST(root);
    free(current_lexeme);
    free(buffer);
    return 0;
}