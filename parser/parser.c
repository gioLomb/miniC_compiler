#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../tokens.h"
#include "../lexer.h"
#include "../arena.h"
#include "error.h"
#include "ast.h"
#include "parser.h"

static int   current_token;
static const char *current_lexeme = NULL;

static Arena *astArena    = NULL;
static Arena *scratchArena = NULL;

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

#define NEW_NODE(kind, text) newNode(astArena, (kind), (text))

static void advance(void) {
    current_token = lexer_next_token();
    current_lexeme = lexer_current_lexeme();
}

static void match(int expected) {
    if (current_token == expected) {
        advance();
    } else {
        reportError(lexer_current_line(),
                    "atteso token %d, trovato '%s' (token %d)",
                    expected, current_lexeme, current_token);
    }
}

static void synchronize(void) {
    advance();
    while (current_token != TOK_EOF) {
        if (current_token == TOK_DEL_SEMICOLON) { advance(); return; }
        switch (current_token) {
            case TOK_DEL_RBRACE:
            case TOK_KW_ELSE:
            case TOK_KW_WHILE:
            case TOK_KW_IF:
            case TOK_KW_RETURN:
            case TOK_KW_INT:
            case TOK_KW_FLOAT:
                return;
        }
        advance();
    }
}

ASTNode *ParseProgram(Arena *arena) {
    astArena    = arena;
    scratchArena = arena_create(0);

    advance();

    ASTNode *node = NEW_NODE(ND_PROGRAM, NULL);
    while (current_token != TOK_EOF) {
        addChild(astArena, node, ParseStmtInner());
        if (hadAnyError()) { synchronize(); clearError(); }
    }

    arena_destroy(scratchArena);
    scratchArena = NULL;
    astArena     = NULL;
    return node;
}

static ASTNode *ParseBlockInner(void) {
    match(TOK_DEL_LBRACE);
    ASTNode *node = NEW_NODE(ND_BLOCK, NULL);
    while (current_token != TOK_DEL_RBRACE && current_token != TOK_EOF) {
        addChild(astArena, node, ParseStmtInner());
        if (hadAnyError()) { synchronize(); clearError(); }
    }
    match(TOK_DEL_RBRACE);
    return node;
}

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

        ASTNode *node = NEW_NODE(ND_IF, NULL);
        addChild(astArena, node, cond);
        addChild(astArena, node, thenBranch);

        if (current_token == TOK_KW_ELSE) {
            match(TOK_KW_ELSE);
            addChild(astArena, node, ParseStmtInner());
        }
        return node;
    }

    case TOK_KW_WHILE: {
        match(TOK_KW_WHILE);
        match(TOK_DEL_LPAREN);
        ASTNode *cond = ParseExpr();
        match(TOK_DEL_RPAREN);
        ASTNode *body = ParseStmtInner();

        ASTNode *node = NEW_NODE(ND_WHILE, NULL);
        addChild(astArena, node, cond);
        addChild(astArena, node, body);
        return node;
    }

    case TOK_KW_RETURN: {
        match(TOK_KW_RETURN);
        ASTNode *expr = ParseExpr();
        match(TOK_DEL_SEMICOLON);

        ASTNode *node = NEW_NODE(ND_RETURN, NULL);
        addChild(astArena, node, expr);
        return node;
    }

    default: {
        ASTNode *expr = ParseExpr();
        match(TOK_DEL_SEMICOLON);

        ASTNode *node = NEW_NODE(ND_EXPR_STMT, NULL);
        addChild(astArena, node, expr);
        return node;
    }
    }
}

static void ParseParamList(ASTNode *funcNode) {
    while (current_token == TOK_KW_INT || current_token == TOK_KW_FLOAT) {
        char *type = arena_strdup(scratchArena, current_lexeme);
        match(current_token);

        char *name = arena_strdup(scratchArena, current_lexeme);
        match(TOK_ID);

        char *combined = arena_sprintf(astArena, "%s %s", type, name);
        addChild(astArena, funcNode, newNode(astArena, ND_PARAM, combined));

        if (current_token == TOK_DEL_COMMA) match(TOK_DEL_COMMA);
        else break;
    }
}

static ASTNode *ParseDeclaration(void) {
    char *type = arena_strdup(scratchArena, current_lexeme);
    match(current_token);

    char *name = arena_strdup(scratchArena, current_lexeme);
    match(TOK_ID);

    char *combined = arena_sprintf(astArena, "%s %s", type, name);

    if (current_token == TOK_DEL_LPAREN) {
        ASTNode *node = newNode(astArena, ND_FUNC_DECL, combined);
        match(TOK_DEL_LPAREN);
        ParseParamList(node);
        match(TOK_DEL_RPAREN);
        addChild(astArena, node, ParseBlockInner());
        return node;
    }

    if (current_token == TOK_DEL_LBRACK) {
        match(TOK_DEL_LBRACK);
        char *size = arena_strdup(scratchArena, current_lexeme);
        match(TOK_NUM_INT);
        match(TOK_DEL_RBRACK);

        char *arrDecl = arena_sprintf(astArena, "%s[%s]", combined, size);
        ASTNode *node = newNode(astArena, ND_VAR_DECL, arrDecl);

        if (current_token == TOK_OP_ASSIGN) {
            match(TOK_OP_ASSIGN);
            match(TOK_DEL_LBRACE);
            if (current_token != TOK_DEL_RBRACE) {
                addChild(astArena, node, ParseExpr());
                while (current_token == TOK_DEL_COMMA) {
                    match(TOK_DEL_COMMA);
                    addChild(astArena, node, ParseExpr());
                }
            }
            match(TOK_DEL_RBRACE);
        }

        match(TOK_DEL_SEMICOLON);
        return node;
    }

    ASTNode *node = newNode(astArena, ND_VAR_DECL, combined);
    if (current_token == TOK_OP_ASSIGN) {
        match(TOK_OP_ASSIGN);
        addChild(astArena, node, ParseExpr());
    }
    match(TOK_DEL_SEMICOLON);
    return node;
}

static ASTNode *ParseExpr(void) { return ParseAssign(); }

static ASTNode *ParseAssign(void) {
    ASTNode *left = ParseLogicOr();
    if (current_token == TOK_OP_ASSIGN) {
        match(TOK_OP_ASSIGN);
        ASTNode *right = ParseAssign();
        ASTNode *node  = NEW_NODE(ND_ASSIGN, "=");
        addChild(astArena, node, left);
        addChild(astArena, node, right);
        return node;
    }
    return left;
}

static ASTNode *ParseLogicOr(void) {
    ASTNode *left = ParseLogicAnd();
    while (current_token == TOK_OP_OR) {
        char *op = arena_strdup(scratchArena, current_lexeme);
        match(current_token);
        ASTNode *right = ParseLogicAnd();
        ASTNode *node  = newNode(astArena, ND_BINOP, op);
        addChild(astArena, node, left);
        addChild(astArena, node, right);
        left = node;
    }
    return left;
}

static ASTNode *ParseLogicAnd(void) {
    ASTNode *left = ParseEquality();
    while (current_token == TOK_OP_AND) {
        char *op = arena_strdup(scratchArena, current_lexeme);
        match(current_token);
        ASTNode *right = ParseEquality();
        ASTNode *node  = newNode(astArena, ND_BINOP, op);
        addChild(astArena, node, left);
        addChild(astArena, node, right);
        left = node;
    }
    return left;
}

static ASTNode *ParseEquality(void) {
    ASTNode *left = ParseRelational();
    while (current_token == TOK_OP_EQ || current_token == TOK_OP_NE) {
        char *op = arena_strdup(scratchArena, current_lexeme);
        match(current_token);
        ASTNode *right = ParseRelational();
        ASTNode *node  = newNode(astArena, ND_BINOP, op);
        addChild(astArena, node, left);
        addChild(astArena, node, right);
        left = node;
    }
    return left;
}

static ASTNode *ParseRelational(void) {
    ASTNode *left = ParseAdditive();
    while (current_token == TOK_OP_LT || current_token == TOK_OP_GT ||
           current_token == TOK_OP_LE || current_token == TOK_OP_GE) {
        char *op = arena_strdup(scratchArena, current_lexeme);
        match(current_token);
        ASTNode *right = ParseAdditive();
        ASTNode *node  = newNode(astArena, ND_BINOP, op);
        addChild(astArena, node, left);
        addChild(astArena, node, right);
        left = node;
    }
    return left;
}

static ASTNode *ParseAdditive(void) {
    ASTNode *left = ParseTerm();
    while (current_token == TOK_OP_PLUS || current_token == TOK_OP_MINUS) {
        char *op = arena_strdup(scratchArena, current_lexeme);
        match(current_token);
        ASTNode *right = ParseTerm();
        ASTNode *node  = newNode(astArena, ND_BINOP, op);
        addChild(astArena, node, left);
        addChild(astArena, node, right);
        left = node;
    }
    return left;
}

static ASTNode *ParseTerm(void) {
    ASTNode *left = ParseUnary();
    while (current_token == TOK_OP_MUL || current_token == TOK_OP_DIV ||
           current_token == TOK_OP_MOD) {
        char *op = arena_strdup(scratchArena, current_lexeme);
        match(current_token);
        ASTNode *right = ParseUnary();
        ASTNode *node  = newNode(astArena, ND_BINOP, op);
        addChild(astArena, node, left);
        addChild(astArena, node, right);
        left = node;
    }
    return left;
}

static ASTNode *ParseUnary(void) {
    if (current_token == TOK_OP_NOT || current_token == TOK_OP_MINUS) {
        char *op = arena_strdup(scratchArena, current_lexeme);
        match(current_token);
        ASTNode *operand = ParseUnary();
        ASTNode *node    = newNode(astArena, ND_UNARY, op);
        addChild(astArena, node, operand);
        return node;
    }
    return ParseFactor();
}

static ASTNode *ParseFactor(void) {
    if (current_token == TOK_ID) {
        char *name = arena_strdup(scratchArena, current_lexeme);
        match(TOK_ID);

        ASTNode *node;
        if (current_token == TOK_DEL_LPAREN) {
            match(TOK_DEL_LPAREN);
            node = newNode(astArena, ND_CALL, name);
            if (current_token != TOK_DEL_RPAREN) {
                addChild(astArena, node, ParseExpr());
                while (current_token == TOK_DEL_COMMA) {
                    match(TOK_DEL_COMMA);
                    addChild(astArena, node, ParseExpr());
                }
            }
            match(TOK_DEL_RPAREN);
        } else if (current_token == TOK_DEL_LBRACK) {
            match(TOK_DEL_LBRACK);
            ASTNode *index = ParseExpr();
            match(TOK_DEL_RBRACK);
            node = newNode(astArena, ND_ARRAY_ACCESS, name);
            addChild(astArena, node, index);
        } else {
            node = newNode(astArena, ND_ID, name);
        }
        return node;

    } else if (current_token == TOK_NUM_INT) {
        ASTNode *node = newNode(astArena, ND_NUM_INT, current_lexeme);
        match(TOK_NUM_INT);
        return node;

    } else if (current_token == TOK_NUM_FLOAT) {
        ASTNode *node = newNode(astArena, ND_NUM_FLOAT, current_lexeme);
        match(TOK_NUM_FLOAT);
        return node;

    } else if (current_token == TOK_DEL_LPAREN) {
        match(TOK_DEL_LPAREN);
        ASTNode *node = ParseExpr();
        match(TOK_DEL_RPAREN);
        return node;
    }

    reportError(lexer_current_line(),
                "token inatteso %d in un'espressione", current_token);
    return newNode(astArena, ND_ERROR, "<error>");
}
