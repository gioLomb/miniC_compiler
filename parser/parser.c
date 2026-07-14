#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../tokens.h"
#include "../lexer.h"
#include "error.h"
#include "ast.h"
#include "parser.h"

/* ==================================================================
 * Stato del parser: SOLO bookkeeping del parser stesso (token corrente
 * e suo lessema). Nessun extern verso lo scanner: tutta la comunicazione
 * passa dalle funzioni di lexer.h.
 * ================================================================== */
static int current_token;
static char *current_lexeme = NULL;

/* ---- prototipi interni ---- */
static void advance(void);
static void match(int expected);
static void synchronize(void);

static ASTNode *ParseStmtInner(void);
static ASTNode *ParseBlockInner(void);
static ASTNode *ParseDeclaration(void);
static void     ParseParamList(ASTNode *funcNode);
static ASTNode *ParseExpr(void);
static ASTNode *ParseAssign(void);
static ASTNode *ParseLogicOr(void);
static ASTNode *ParseLogicAnd(void);
static ASTNode *ParseEquality(void);
static ASTNode *ParseRelational(void);
static ASTNode *ParseAdditive(void);
static ASTNode *ParseTerm(void);
static ASTNode *ParseUnary(void);
static ASTNode *ParseFactor(void);

/* ==================================================================
 * Bookkeeping dei token: unica interfaccia verso lexer.h
 * ================================================================== */

static void advance(void) {
    current_token = lexer_next_token();
    free(current_lexeme);
    current_lexeme = strdup(lexer_current_lexeme());
}

/* Consuma il token atteso, oppure segnala un errore (senza avanzare:
   sara' synchronize(), chiamato centralmente da ParseStmt, a farlo). */
static void match(int expected) {
    if (current_token == expected) {
        advance();
    } else {
        reportError(lexer_current_line(), "atteso token %d, trovato '%s' (token %d)",
                    expected, current_lexeme, current_token);
    }
}

/* Recovery "panic mode": scarta token finche' non trova un confine sicuro.
   GARANZIA DI AVANZAMENTO: consuma sempre almeno un token (quello che ha
   causato l'errore) prima di iniziare a cercare il punto di sincronizzazione.
   Senza questa garanzia, un token "di confine" mai consumato in quel punto
   (es. una '}' spuria senza blocco aperto corrispondente) farebbe entrare
   il parser in un loop infinito, perche' ne' match() ne' synchronize()
   avanzerebbero mai oltre quel token. */
static void synchronize(void) {
    advance();   /* garantisce almeno un token di progresso ad ogni chiamata */

    while (current_token != TOK_EOF) {
        if (current_token == TOK_DEL_SEMICOLON) {
            advance();   /* il ';' stesso e' un buon punto di ripartenza: consumalo */
            return;
        }
        switch (current_token) {
            case TOK_DEL_RBRACE:
            case TOK_KW_ELSE:
            case TOK_KW_WHILE:
            case TOK_KW_IF:
            case TOK_KW_RETURN:
            case TOK_KW_INT:
            case TOK_KW_FLOAT:
                return;   /* non consumarlo: e' l'inizio del prossimo costrutto */
        }
        advance();
    }
}

/* ==================================================================
 * Program -> Stmt*
 * ================================================================== */
ASTNode *ParseProgram(void) {
    advance();   /* legge il primo token */

    ASTNode *node = newNode(ND_PROGRAM, NULL);
    while (current_token != TOK_EOF) {
        addChild(node, ParseStmtInner());
        if (hadAnyError()) {
            synchronize();
            clearError();
        }
    }
    return node;
}

/* Block -> '{' Stmt* '}' */
static ASTNode *ParseBlockInner(void) {
    match(TOK_DEL_LBRACE);
    ASTNode *node = newNode(ND_BLOCK, NULL);
    while (current_token != TOK_DEL_RBRACE && current_token != TOK_EOF) {
        addChild(node, ParseStmtInner());
        if (hadAnyError()) {
            synchronize();
            clearError();
        }
    }
    match(TOK_DEL_RBRACE);
    return node;
}

/* Stmt -> Decl | Block | IfStmt | WhileStmt | ReturnStmt | Expr ';' */
static ASTNode *ParseStmtInner(void) {
    switch (current_token) {
        case TOK_KW_INT:
        case TOK_KW_FLOAT:
            return ParseDeclaration();

        case TOK_DEL_LBRACE:
            return ParseBlockInner();

        case TOK_KW_IF: {
            match(TOK_KW_IF);
            match(TOK_DEL_LPAREN);
            ASTNode *cond = ParseExpr();
            match(TOK_DEL_RPAREN);
            ASTNode *thenBranch = ParseStmtInner();

            ASTNode *node = newNode(ND_IF, NULL);
            addChild(node, cond);
            addChild(node, thenBranch);
            if (current_token == TOK_KW_ELSE) {
                match(TOK_KW_ELSE);
                addChild(node, ParseStmtInner()); /* terzo figlio = ramo else, se presente */
            }
            return node;
        }

        case TOK_KW_WHILE: {
            match(TOK_KW_WHILE);
            match(TOK_DEL_LPAREN);
            ASTNode *cond = ParseExpr();
            match(TOK_DEL_RPAREN);
            ASTNode *body = ParseStmtInner();

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

/* ParamList -> (Type ID (',' Type ID)*)? */
static void ParseParamList(ASTNode *funcNode) {
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

/* Decl -> Type ID '(' ParamList ')' Block                        (definizione di funzione)
 *       | Type ID ( '=' Expr )? ';'                                (variabile scalare, con init. opzionale)
 *       | Type ID '[' NUM ']' ( '=' '{' (Expr (',' Expr)*)? '}' )? ';'
 *                                                                   (array, con init. list opzionale)
 *
 * L'inizializzatore, se presente, viene appeso come figlio/figli del nodo
 * ND_VAR_DECL: un solo figlio per lo scalare, zero o piu' figli (uno per
 * elemento) per l'array. Il testo del nodo ("tipo nome" o "tipo nome[size]")
 * resta invariato: chi lo interpreta (symtab_parse_decl_text) non deve
 * sapere nulla dell'inizializzatore.
 */
static ASTNode *ParseDeclaration(void) {
    char type[32];
    snprintf(type, sizeof(type), "%s", current_lexeme);
    match(current_token); /* consuma KW_INT o KW_FLOAT */

    char name[64];
    snprintf(name, sizeof(name), "%s", current_lexeme);
    match(TOK_ID);

    char combined[100];
    snprintf(combined, sizeof(combined), "%s %s", type, name);

    if (current_token == TOK_DEL_LPAREN) {
        ASTNode *node = newNode(ND_FUNC_DECL, combined);
        match(TOK_DEL_LPAREN);
        ParseParamList(node);
        match(TOK_DEL_RPAREN);
        addChild(node, ParseBlockInner());
        return node;
    }

    if (current_token == TOK_DEL_LBRACK) {
        match(TOK_DEL_LBRACK);
        char size[32];
        snprintf(size, sizeof(size), "%s", current_lexeme);
        match(TOK_NUM_INT);
        match(TOK_DEL_RBRACK);

        char arrDecl[140];
        snprintf(arrDecl, sizeof(arrDecl), "%s[%s]", combined, size);
        ASTNode *node = newNode(ND_VAR_DECL, arrDecl);

        if (current_token == TOK_OP_ASSIGN) {
            match(TOK_OP_ASSIGN);
            match(TOK_DEL_LBRACE);
            if (current_token != TOK_DEL_RBRACE) {
                addChild(node, ParseExpr());
                while (current_token == TOK_DEL_COMMA) {
                    match(TOK_DEL_COMMA);
                    addChild(node, ParseExpr());
                }
            }
            match(TOK_DEL_RBRACE);
        }

        match(TOK_DEL_SEMICOLON);
        return node;
    }

    ASTNode *node = newNode(ND_VAR_DECL, combined);
    if (current_token == TOK_OP_ASSIGN) {
        match(TOK_OP_ASSIGN);
        addChild(node, ParseExpr());
    }
    match(TOK_DEL_SEMICOLON);
    return node;
}

/* ==================================================================
 * Gerarchia Espressioni (precedenza crescente):
 * Expr -> Assign
 * Assign -> LogicOr ( '=' Assign )?           (associativa a destra)
 * LogicOr -> LogicAnd ( '||' LogicAnd )*
 * LogicAnd -> Equality ( '&&' Equality )*
 * Equality -> Relational ( ('=='|'!=') Relational )*
 * Relational -> Additive ( ('<'|'>'|'<='|'>=') Additive )*
 * Additive -> Term ( ('+'|'-') Term )*
 * Term -> Unary ( ('*'|'/'|'%') Unary )*
 * Unary -> '!' Unary | '-' Unary | Factor
 * Factor -> ID | NUM | '(' Expr ')' | Call | ArrayAccess
 * ================================================================== */

static ASTNode *ParseExpr(void) {
    return ParseAssign();
}

static ASTNode *ParseAssign(void) {
    ASTNode *left = ParseLogicOr();
    if (current_token == TOK_OP_ASSIGN) {
        match(TOK_OP_ASSIGN);
        ASTNode *right = ParseAssign();
        ASTNode *node = newNode(ND_ASSIGN, "=");
        addChild(node, left);
        addChild(node, right);
        return node;
    }
    return left;
}

static ASTNode *ParseLogicOr(void) {
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

static ASTNode *ParseLogicAnd(void) {
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

static ASTNode *ParseEquality(void) {
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

static ASTNode *ParseRelational(void) {
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

static ASTNode *ParseAdditive(void) {
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

static ASTNode *ParseTerm(void) {
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

/* Unary -> '!' Unary | '-' Unary | Factor */
static ASTNode *ParseUnary(void) {
    if (current_token == TOK_OP_NOT || current_token == TOK_OP_MINUS) {
        char *op = strdup(current_lexeme);
        match(current_token);
        ASTNode *operand = ParseUnary();
        ASTNode *node = newNode(ND_UNARY, op);
        addChild(node, operand);
        free(op);
        return node;
    }
    return ParseFactor();
}

/* Factor -> ID ( '(' (Expr (',' Expr)*)? ')' | '[' Expr ']' )?
 *         | NUM_INT | NUM_FLOAT | '(' Expr ')'
 */
static ASTNode *ParseFactor(void) {
    if (current_token == TOK_ID) {
        char *name = strdup(current_lexeme);
        match(TOK_ID);

        ASTNode *node;
        if (current_token == TOK_DEL_LPAREN) {
            match(TOK_DEL_LPAREN);
            node = newNode(ND_CALL, name);
            if (current_token != TOK_DEL_RPAREN) {
                addChild(node, ParseExpr());
                while (current_token == TOK_DEL_COMMA) {
                    match(TOK_DEL_COMMA);
                    addChild(node, ParseExpr());
                }
            }
            match(TOK_DEL_RPAREN);
        } else if (current_token == TOK_DEL_LBRACK) {
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

    /* Nessun recovery qui dentro: si limita a segnalare l'errore (soppresso
       se gia' segnalato per questo statement, vedi error.c) e restituire
       un nodo <error>. Il recovery vero e proprio avviene UNA SOLA VOLTA,
       centralmente, in ParseProgram/ParseBlockInner dopo che lo statement
       e' stato costruito per intero - vedi il commento sopra ParseStmtInner. */
    reportError(lexer_current_line(), "token inatteso %d in un'espressione", current_token);
    return newNode(ND_ERROR, "<error>");
}
