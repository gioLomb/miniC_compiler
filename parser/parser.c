#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../tokens.h"
#include "../lexer.h"
#include "../arena.h"
#include "error.h"
#include "ast.h"
#include "parser.h"

// Parser state variables
static int   current_token;
static char *current_lexeme = NULL;

// astArena: persistent memory arena for AST node strings
// scratchArena: temporary memory arena for internal parsing allocations
static Arena *astArena    = NULL;
static Arena *scratchArena = NULL;

// Internal forward declarations
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

// Helper macro to construct a new AST node allocating its text in the persistent astArena
#define NEW_NODE(kind, text) newNode(astArena, (kind), (text))

// Token management helper: advances the lexer state to the next token
static void advance(void) {
    current_token = lexer_next_token();
    free(current_lexeme);
    current_lexeme = strdup(lexer_current_lexeme());
}

// Matches expected token type or reports a syntax error
static void match(int expected) {
    if (current_token == expected) {
        advance();
    } else {
        reportError(lexer_current_line(),
                    "atteso token %d, trovato '%s' (token %d)",
                    expected, current_lexeme, current_token);
    }
}

// Panic-mode error recovery: skips tokens until reaching a synchronization boundary
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
    // Initialize external persistent arena and create internal scratch arena
    astArena    = arena;
    scratchArena = arena_create(0);

    // Fetch initial token from lexer stream
    advance();

    // Create root node and parse statements until EOF
    ASTNode *node = NEW_NODE(ND_PROGRAM, NULL);
    while (current_token != TOK_EOF) {
        addChild(node, ParseStmtInner());
        if (hadAnyError()) { synchronize(); clearError(); }
    }

    // Clean up temporary internal memory arena and clear static references
    arena_destroy(scratchArena);
    scratchArena = NULL;
    astArena     = NULL;
    return node;
}

// Parses block statements enclosed in braces: Block -> '{' Stmt* '}'
static ASTNode *ParseBlockInner(void) {
    match(TOK_DEL_LBRACE);
    ASTNode *node = NEW_NODE(ND_BLOCK, NULL);
    while (current_token != TOK_DEL_RBRACE && current_token != TOK_EOF) {
        addChild(node, ParseStmtInner());
        if (hadAnyError()) { synchronize(); clearError(); }
    }
    match(TOK_DEL_RBRACE);
    return node;
}

// Parses general statement types (declarations, blocks, control structures, expressions)
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
        addChild(node, cond);
        addChild(node, thenBranch);

        // Optional else clause processing
        if (current_token == TOK_KW_ELSE) {
            match(TOK_KW_ELSE);
            addChild(node, ParseStmtInner());
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
        addChild(node, cond);
        addChild(node, body);
        return node;
    }

    case TOK_KW_RETURN: {
        match(TOK_KW_RETURN);
        ASTNode *expr = ParseExpr();
        match(TOK_DEL_SEMICOLON);

        ASTNode *node = NEW_NODE(ND_RETURN, NULL);
        addChild(node, expr);
        return node;
    }

    default: {
        // Fallback to expression statement
        ASTNode *expr = ParseExpr();
        match(TOK_DEL_SEMICOLON);

        ASTNode *node = NEW_NODE(ND_EXPR_STMT, NULL);
        addChild(node, expr);
        return node;
    }
    }
}

// Parses parameter lists inside function declarations
static void ParseParamList(ASTNode *funcNode) {
    while (current_token == TOK_KW_INT || current_token == TOK_KW_FLOAT) {
        // Temporarily store type and parameter name in scratch arena
        char *type = arena_strdup(scratchArena, current_lexeme);
        match(current_token);

        char *name = arena_strdup(scratchArena, current_lexeme);
        match(TOK_ID);

        // Format combined string "type name" directly into astArena for permanent storage
        char *combined = arena_sprintf(astArena, "%s %s", type, name);
        addChild(funcNode, newNode(astArena, ND_PARAM, combined));

        if (current_token == TOK_DEL_COMMA) match(TOK_DEL_COMMA);
        else break;
    }
}

// Parses variable, function, or array declarations
static ASTNode *ParseDeclaration(void) {
    // Store type and identifier lexemes temporarily
    char *type = arena_strdup(scratchArena, current_lexeme);
    match(current_token);

    char *name = arena_strdup(scratchArena, current_lexeme);
    match(TOK_ID);

    // Format combined string "type name" into persistent AST memory arena
    char *combined = arena_sprintf(astArena, "%s %s", type, name);

    // Handle function declaration
    if (current_token == TOK_DEL_LPAREN) {
        ASTNode *node = newNode(astArena, ND_FUNC_DECL, combined);
        match(TOK_DEL_LPAREN);
        ParseParamList(node);
        match(TOK_DEL_RPAREN);
        addChild(node, ParseBlockInner());
        return node;
    }

    // Handle array declaration
    if (current_token == TOK_DEL_LBRACK) {
        match(TOK_DEL_LBRACK);
        char *size = arena_strdup(scratchArena, current_lexeme);
        match(TOK_NUM_INT);
        match(TOK_DEL_RBRACK);

        // Build array formatted string "type name[size]" for persistent node text
        char *arrDecl = arena_sprintf(astArena, "%s[%s]", combined, size);
        ASTNode *node = newNode(astArena, ND_VAR_DECL, arrDecl);

        // Process initializer list if present
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

    // Handle standard variable declaration (with optional single assignment)
    ASTNode *node = newNode(astArena, ND_VAR_DECL, combined);
    if (current_token == TOK_OP_ASSIGN) {
        match(TOK_OP_ASSIGN);
        addChild(node, ParseExpr());
    }
    match(TOK_DEL_SEMICOLON);
    return node;
}

// Expression parser entry point
static ASTNode *ParseExpr(void) { return ParseAssign(); }

// Parses right-associative assignment expressions
static ASTNode *ParseAssign(void) {
    ASTNode *left = ParseLogicOr();
    if (current_token == TOK_OP_ASSIGN) {
        match(TOK_OP_ASSIGN);
        ASTNode *right = ParseAssign();
        ASTNode *node  = NEW_NODE(ND_ASSIGN, "=");
        addChild(node, left);
        addChild(node, right);
        return node;
    }
    return left;
}

// Parses logical OR expressions ('||')
static ASTNode *ParseLogicOr(void) {
    ASTNode *left = ParseLogicAnd();
    while (current_token == TOK_OP_OR) {
        char *op = arena_strdup(scratchArena, current_lexeme);
        match(current_token);
        ASTNode *right = ParseLogicAnd();
        ASTNode *node  = newNode(astArena, ND_BINOP, op);
        addChild(node, left);
        addChild(node, right);
        left = node;
    }
    return left;
}

// Parses logical AND expressions ('&&')
static ASTNode *ParseLogicAnd(void) {
    ASTNode *left = ParseEquality();
    while (current_token == TOK_OP_AND) {
        char *op = arena_strdup(scratchArena, current_lexeme);
        match(current_token);
        ASTNode *right = ParseEquality();
        ASTNode *node  = newNode(astArena, ND_BINOP, op);
        addChild(node, left);
        addChild(node, right);
        left = node;
    }
    return left;
}

// Parses equality relational expressions ('==' and '!=')
static ASTNode *ParseEquality(void) {
    ASTNode *left = ParseRelational();
    while (current_token == TOK_OP_EQ || current_token == TOK_OP_NE) {
        char *op = arena_strdup(scratchArena, current_lexeme);
        match(current_token);
        ASTNode *right = ParseRelational();
        ASTNode *node  = newNode(astArena, ND_BINOP, op);
        addChild(node, left);
        addChild(node, right);
        left = node;
    }
    return left;
}

// Parses order relational expressions ('<', '>', '<=', '>=')
static ASTNode *ParseRelational(void) {
    ASTNode *left = ParseAdditive();
    while (current_token == TOK_OP_LT || current_token == TOK_OP_GT ||
           current_token == TOK_OP_LE || current_token == TOK_OP_GE) {
        char *op = arena_strdup(scratchArena, current_lexeme);
        match(current_token);
        ASTNode *right = ParseAdditive();
        ASTNode *node  = newNode(astArena, ND_BINOP, op);
        addChild(node, left);
        addChild(node, right);
        left = node;
    }
    return left;
}

// Parses additive arithmetic expressions ('+' and '-')
static ASTNode *ParseAdditive(void) {
    ASTNode *left = ParseTerm();
    while (current_token == TOK_OP_PLUS || current_token == TOK_OP_MINUS) {
        char *op = arena_strdup(scratchArena, current_lexeme);
        match(current_token);
        ASTNode *right = ParseTerm();
        ASTNode *node  = newNode(astArena, ND_BINOP, op);
        addChild(node, left);
        addChild(node, right);
        left = node;
    }
    return left;
}

// Parses multiplicative arithmetic expressions ('*', '/', '%')
static ASTNode *ParseTerm(void) {
    ASTNode *left = ParseUnary();
    while (current_token == TOK_OP_MUL || current_token == TOK_OP_DIV ||
           current_token == TOK_OP_MOD) {
        char *op = arena_strdup(scratchArena, current_lexeme);
        match(current_token);
        ASTNode *right = ParseUnary();
        ASTNode *node  = newNode(astArena, ND_BINOP, op);
        addChild(node, left);
        addChild(node, right);
        left = node;
    }
    return left;
}

// Parses unary operators ('!' and unary '-')
static ASTNode *ParseUnary(void) {
    if (current_token == TOK_OP_NOT || current_token == TOK_OP_MINUS) {
        char *op = arena_strdup(scratchArena, current_lexeme);
        match(current_token);
        ASTNode *operand = ParseUnary();
        ASTNode *node    = newNode(astArena, ND_UNARY, op);
        addChild(node, operand);
        return node;
    }
    return ParseFactor();
}

// Parses factor expressions (identifiers, function calls, array indexings, literals, parenthesized expressions)
static ASTNode *ParseFactor(void) {
    if (current_token == TOK_ID) {
        char *name = arena_strdup(scratchArena, current_lexeme);
        match(TOK_ID);

        ASTNode *node;
        // Parse function call expression
        if (current_token == TOK_DEL_LPAREN) {
            match(TOK_DEL_LPAREN);
            node = newNode(astArena, ND_CALL, name);
            if (current_token != TOK_DEL_RPAREN) {
                addChild(node, ParseExpr());
                while (current_token == TOK_DEL_COMMA) {
                    match(TOK_DEL_COMMA);
                    addChild(node, ParseExpr());
                }
            }
            match(TOK_DEL_RPAREN);
        // Parse array indexing expression
        } else if (current_token == TOK_DEL_LBRACK) {
            match(TOK_DEL_LBRACK);
            ASTNode *index = ParseExpr();
            match(TOK_DEL_RBRACK);
            node = newNode(astArena, ND_ARRAY_ACCESS, name);
            addChild(node, index);
        // Parse simple identifier variable reference
        } else {
            node = newNode(astArena, ND_ID, name);
        }
        return node;

    // Parse integer numeric literal
    } else if (current_token == TOK_NUM_INT) {
        ASTNode *node = newNode(astArena, ND_NUM_INT, current_lexeme);
        match(TOK_NUM_INT);
        return node;

    // Parse floating point numeric literal
    } else if (current_token == TOK_NUM_FLOAT) {
        ASTNode *node = newNode(astArena, ND_NUM_FLOAT, current_lexeme);
        match(TOK_NUM_FLOAT);
        return node;

    // Parse sub-expression within parentheses
    } else if (current_token == TOK_DEL_LPAREN) {
        match(TOK_DEL_LPAREN);
        ASTNode *node = ParseExpr();
        match(TOK_DEL_RPAREN);
        return node;
    }

    // Handle unexpected token syntax error
    reportError(lexer_current_line(),
                "token inatteso %d in un'espressione", current_token);
    return newNode(astArena, ND_ERROR, "<error>");
}