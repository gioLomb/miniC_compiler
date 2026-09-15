#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../tokens.h"
#include "../lexer.h"
#include "../arena.h"
#include "errorCollector.h"
#include "ast.h"
#include "parser.h"

/* =========================================================================
 * Parser state encapsulation
 * ========================================================================= */

/**
 * @brief Parser context holding all state needed during parsing.
 */
typedef struct Parser {
    int         current_token;    /**< Current token type */
    const char *current_lexeme;   /**< Lexeme of the current token */
    Arena      *ast_arena;        /**< Arena for AST node allocation */
    Arena      *scratch_arena;    /**< Temporary arena for string duplications */
} Parser;

/* Forward declarations of internal parsing functions */
static void advance(Parser *p);
static void match(Parser *p, int expected);
static void synchronize(Parser *p);

static ASTNode *parse_statement(Parser *p);
static ASTNode *parse_block(Parser *p);
static ASTNode *parse_declaration(Parser *p);
static ASTNode *parse_variable_declaration(Parser *p, const char *type, const char *name);
static ASTNode *parse_array_declaration(Parser *p, const char *type, const char *name);
static ASTNode *parse_function_declaration(Parser *p, const char *type, const char *name);
static void     parse_parameter_list(Parser *p, ASTNode *func_node);
static ASTNode *parse_if_statement(Parser *p);
static ASTNode *parse_while_statement(Parser *p);
static ASTNode *parse_return_statement(Parser *p);
static ASTNode *parse_expression_statement(Parser *p);

static ASTNode *parse_expr(Parser *p);
static ASTNode *parse_assign(Parser *p);
static ASTNode *parse_logic_or(Parser *p);
static ASTNode *parse_logic_and(Parser *p);
static ASTNode *parse_equality(Parser *p);
static ASTNode *parse_relational(Parser *p);
static ASTNode *parse_additive(Parser *p);
static ASTNode *parse_term(Parser *p);
static ASTNode *parse_unary(Parser *p);
static ASTNode *parse_factor(Parser *p);
static ASTNode *parse_call(Parser *p, const char *name);
static ASTNode *parse_array_access(Parser *p, const char *name);
static ASTNode *parse_parenthesized(Parser *p);


/**
 * @brief Consumes the next token and updates parser state.
 *
 * @param p Parser context.
 */
static void advance(Parser *p) {
    p->current_token = lexer_next_token();
    p->current_lexeme = lexer_current_lexeme();
}

/**
 * @brief Checks expected token; advances if match, else reports error.
 *
 * @param p        Parser context.
 * @param expected Expected token type.
 */
static void match(Parser *p, int expected) {
    if (p->current_token == expected) {
        advance(p);
    } else {
        // Report error with line info and current token details; suppressed
        // if a cascading error was already flagged for this statement.
        ec_report_cascading(lexer_current_line(),
                    "atteso token %d, trovato '%s' (token %d)",
                    expected, p->current_lexeme, p->current_token);
    }
}

/**
 * @brief Panic-mode synchronisation after an error.
 *
 * Skips tokens until a statement boundary (semicolon, '}', or keyword)
 * is found, allowing the parser to recover and continue.
 *
 * @param p Parser context.
 */
static void synchronize(Parser *p) {
    advance(p); // Skip the erroneous token.
    while (p->current_token != TOK_EOF) {
        // Semicolon marks the end of a statement; safe to stop here.
        if (p->current_token == TOK_DEL_SEMICOLON) {
            advance(p);
            return;
        }
        // Statement boundaries and keywords that can start a new statement.
        switch (p->current_token) {
            case TOK_DEL_RBRACE:
            case TOK_KW_ELSE:
            case TOK_KW_WHILE:
            case TOK_KW_IF:
            case TOK_KW_RETURN:
            case TOK_KW_INT:
            case TOK_KW_FLOAT:
                return;
            default:
                advance(p);
                break;
        }
    }
}


/**
 * @brief Parses the entire program.
 *
 * Initialises the parser context with the provided AST arena and a
 * private scratch arena, then parses declarations/statements until EOF.
 *
 * @param arena Arena to use for AST node allocation.
 * @return Root AST node (ND_PROGRAM) on success, or NULL on failure.
 */
ASTNode *ParseProgram(Arena *arena) {
    Parser parser;
    parser.ast_arena = arena;
    parser.scratch_arena = arena_create(0);
    advance(&parser);

    ASTNode *program_node = NEW_NODE(&parser, ND_PROGRAM, NULL);

    // Parse declarations and statements until we hit EOF.
    while (parser.current_token != TOK_EOF) {
        addChild(parser.ast_arena, program_node, parse_statement(&parser));
        if (ec_pending_error()) {
            // Recover from error and clear the pending-error flag.
            synchronize(&parser);
            ec_clear_pending();
        }
    }

    arena_destroy(parser.scratch_arena);
    return program_node;
}

/* =========================================================================
 * Statement parsing (dispatcher)
 * ========================================================================= */

/**
 * @brief Dispatches to the appropriate statement or declaration parser.
 *
 * @param p Parser context.
 * @return AST node for the statement.
 */
static ASTNode *parse_statement(Parser *p) {
    // Dispatch based on the first token of the statement.
    switch (p->current_token) {
        case TOK_KW_INT:
        case TOK_KW_FLOAT:
            return parse_declaration(p);
        case TOK_DEL_LBRACE:
            return parse_block(p);
        case TOK_KW_IF:
            return parse_if_statement(p);
        case TOK_KW_WHILE:
            return parse_while_statement(p);
        case TOK_KW_RETURN:
            return parse_return_statement(p);
        default:
            return parse_expression_statement(p);
    }
}

/* =========================================================================
 * Block parsing
 * ========================================================================= */

/**
 * @brief Parses a block: '{' statement* '}'.
 *
 * @param p Parser context.
 * @return ND_BLOCK node.
 */
static ASTNode *parse_block(Parser *p) {
    match(p, TOK_DEL_LBRACE);
    ASTNode *block_node = NEW_NODE(p, ND_BLOCK, NULL);

    // Parse statements until we hit the closing brace or EOF.
    while (p->current_token != TOK_DEL_RBRACE && p->current_token != TOK_EOF) {
        addChild(p->ast_arena, block_node, parse_statement(p));
        if (ec_pending_error()) {
            synchronize(p);
            ec_clear_pending();
        }
    }
    match(p, TOK_DEL_RBRACE);
    return block_node;
}

/* =========================================================================
 * Declaration parsing (dispatcher and helpers)
 * ========================================================================= */

/**
 * @brief Parses a declaration (variable, array, or function).
 *
 * Reads the type and identifier, then delegates to the appropriate
 * declaration parser based on the following token.
 *
 * @param p Parser context.
 * @return ND_VAR_DECL, ND_FUNC_DECL, or ND_VAR_DECL (for array).
 */
static ASTNode *parse_declaration(Parser *p) {
    // Read type and identifier name.
    char *type = arena_strdup(p->scratch_arena, p->current_lexeme);
    match(p, p->current_token);
    char *name = arena_strdup(p->scratch_arena, p->current_lexeme);
    match(p, TOK_ID);

    // Dispatch based on what follows the name.
    if (p->current_token == TOK_DEL_LPAREN) {
        // Function declaration: foo( ...
        return parse_function_declaration(p, type, name);
    } else if (p->current_token == TOK_DEL_LBRACK) {
        // Array declaration: foo[ ...
        return parse_array_declaration(p, type, name);
    } else {
        // Simple variable declaration.
        return parse_variable_declaration(p, type, name);
    }
}

/**
 * @brief Parses a simple variable declaration: 'name [= expr] ;'
 *
 * @param p    Parser context.
 * @param type Type string.
 * @param name Variable name.
 * @return ND_VAR_DECL node.
 */
static ASTNode *parse_variable_declaration(Parser *p, const char *type, const char *name) {
    char *decl_text = arena_sprintf(p->ast_arena, "%s %s", type, name);
    ASTNode *var_node = newNode(p->ast_arena, ND_VAR_DECL, decl_text);

    // Optional initializer.
    if (p->current_token == TOK_OP_ASSIGN) {
        match(p, TOK_OP_ASSIGN);
        addChild(p->ast_arena, var_node, parse_expr(p));
    }
    match(p, TOK_DEL_SEMICOLON);
    return var_node;
}

/**
 * @brief Parses an array declaration: 'name[size] [= { expr, ... }] ;'
 *
 * @param p    Parser context.
 * @param type Type string.
 * @param name Array name.
 * @return ND_VAR_DECL node (reused for arrays).
 */
static ASTNode *parse_array_declaration(Parser *p, const char *type, const char *name) {
    match(p, TOK_DEL_LBRACK);
    char *size = arena_strdup(p->scratch_arena, p->current_lexeme);
    match(p, TOK_NUM_INT);
    match(p, TOK_DEL_RBRACK);

    char *decl_text = arena_sprintf(p->ast_arena, "%s %s[%s]", type, name, size);
    ASTNode *arr_node = newNode(p->ast_arena, ND_VAR_DECL, decl_text);

    // Parse optional initializer list: = { expr, ... }
    if (p->current_token == TOK_OP_ASSIGN) {
        match(p, TOK_OP_ASSIGN);
        match(p, TOK_DEL_LBRACE);
        // Empty initializer list is allowed: { }
        if (p->current_token != TOK_DEL_RBRACE) {
            addChild(p->ast_arena, arr_node, parse_expr(p));
            // Parse comma-separated initializer expressions.
            while (p->current_token == TOK_DEL_COMMA) {
                match(p, TOK_DEL_COMMA);
                addChild(p->ast_arena, arr_node, parse_expr(p));
            }
        }
        match(p, TOK_DEL_RBRACE);
    }

    match(p, TOK_DEL_SEMICOLON);
    return arr_node;
}

/**
 * @brief Parses a function declaration: 'name(params) block'
 *
 * @param p    Parser context.
 * @param type Return type string.
 * @param name Function name.
 * @return ND_FUNC_DECL node.
 */
static ASTNode *parse_function_declaration(Parser *p, const char *type, const char *name) {
    char *decl_text = arena_sprintf(p->ast_arena, "%s %s", type, name);
    ASTNode *func_node = newNode(p->ast_arena, ND_FUNC_DECL, decl_text);

    match(p, TOK_DEL_LPAREN);
    parse_parameter_list(p, func_node);
    match(p, TOK_DEL_RPAREN);
    addChild(p->ast_arena, func_node, parse_block(p));
    return func_node;
}

/**
 * @brief Parses function parameter list: 'type id' (',' 'type id')*
 *
 * Each parameter is stored as a ND_PARAM child of the function node.
 *
 * @param p          Parser context.
 * @param func_node  Function declaration node to attach parameters to.
 */
static void parse_parameter_list(Parser *p, ASTNode *func_node) {
    // Parse zero or more parameters separated by commas.
    while (p->current_token == TOK_KW_INT || p->current_token == TOK_KW_FLOAT) {
        char *type = arena_strdup(p->scratch_arena, p->current_lexeme);
        match(p, p->current_token);
        char *name = arena_strdup(p->scratch_arena, p->current_lexeme);
        match(p, TOK_ID);

        char *param_text = arena_sprintf(p->ast_arena, "%s %s", type, name);
        addChild(p->ast_arena, func_node, newNode(p->ast_arena, ND_PARAM, param_text));

        // Continue if there's a comma, otherwise stop.
        if (p->current_token == TOK_DEL_COMMA)
            match(p, TOK_DEL_COMMA);
        else
            break;
    }
}

/* =========================================================================
 * Specific statement parsers (if, while, return, expression)
 * ========================================================================= */

/**
 * @brief Parses an if-else statement.
 *
 * @param p Parser context.
 * @return ND_IF node.
 */
static ASTNode *parse_if_statement(Parser *p) {
    match(p, TOK_KW_IF);
    match(p, TOK_DEL_LPAREN);
    ASTNode *cond = parse_expr(p);
    match(p, TOK_DEL_RPAREN);
    ASTNode *then_branch = parse_statement(p);

    ASTNode *if_node = NEW_NODE(p, ND_IF, NULL);
    addChild(p->ast_arena, if_node, cond);
    addChild(p->ast_arena, if_node, then_branch);

    // Optional else clause.
    if (p->current_token == TOK_KW_ELSE) {
        match(p, TOK_KW_ELSE);
        addChild(p->ast_arena, if_node, parse_statement(p));
    }
    return if_node;
}

/**
 * @brief Parses a while loop.
 *
 * @param p Parser context.
 * @return ND_WHILE node.
 */
static ASTNode *parse_while_statement(Parser *p) {
    match(p, TOK_KW_WHILE);
    match(p, TOK_DEL_LPAREN);
    ASTNode *cond = parse_expr(p);
    match(p, TOK_DEL_RPAREN);
    ASTNode *body = parse_statement(p);

    ASTNode *while_node = NEW_NODE(p, ND_WHILE, NULL);
    addChild(p->ast_arena, while_node, cond);
    addChild(p->ast_arena, while_node, body);
    return while_node;
}

/**
 * @brief Parses a return statement.
 *
 * @param p Parser context.
 * @return ND_RETURN node.
 */
static ASTNode *parse_return_statement(Parser *p) {
    match(p, TOK_KW_RETURN);
    ASTNode *expr = parse_expr(p);
    match(p, TOK_DEL_SEMICOLON);

    ASTNode *ret_node = NEW_NODE(p, ND_RETURN, NULL);
    addChild(p->ast_arena, ret_node, expr);
    return ret_node;
}

/**
 * @brief Parses an expression statement (expression followed by semicolon).
 *
 * @param p Parser context.
 * @return ND_EXPR_STMT node.
 */
static ASTNode *parse_expression_statement(Parser *p) {
    ASTNode *expr = parse_expr(p);
    match(p, TOK_DEL_SEMICOLON);

    ASTNode *expr_stmt_node = NEW_NODE(p, ND_EXPR_STMT, NULL);
    addChild(p->ast_arena, expr_stmt_node, expr);
    return expr_stmt_node;
}

/* =========================================================================
 * Expression parsing (recursive descent, operator precedence)
 * ========================================================================= */

static ASTNode *parse_expr(Parser *p) {
    return parse_assign(p);
}

static ASTNode *parse_assign(Parser *p) {
    ASTNode *left = parse_logic_or(p);
    if (p->current_token == TOK_OP_ASSIGN) {
        // Assignment is right-associative: parse the RHS with parse_assign.
        match(p, TOK_OP_ASSIGN);
        ASTNode *right = parse_assign(p);
        ASTNode *node = NEW_NODE(p, ND_ASSIGN, "=");
        addChild(p->ast_arena, node, left);
        addChild(p->ast_arena, node, right);
        return node;
    }
    return left;
}

static ASTNode *parse_logic_or(Parser *p) {
    ASTNode *left = parse_logic_and(p);
    while (p->current_token == TOK_OP_OR) {
        char *op = arena_strdup(p->scratch_arena, p->current_lexeme);
        match(p, p->current_token);
        ASTNode *right = parse_logic_and(p);
        ASTNode *node = newNode(p->ast_arena, ND_BINOP, op);
        addChild(p->ast_arena, node, left);
        addChild(p->ast_arena, node, right);
        left = node;
    }
    return left;
}

static ASTNode *parse_logic_and(Parser *p) {
    ASTNode *left = parse_equality(p);
    while (p->current_token == TOK_OP_AND) {
        char *op = arena_strdup(p->scratch_arena, p->current_lexeme);
        match(p, p->current_token);
        ASTNode *right = parse_equality(p);
        ASTNode *node = newNode(p->ast_arena, ND_BINOP, op);
        addChild(p->ast_arena, node, left);
        addChild(p->ast_arena, node, right);
        left = node;
    }
    return left;
}

static ASTNode *parse_equality(Parser *p) {
    ASTNode *left = parse_relational(p);
    while (p->current_token == TOK_OP_EQ || p->current_token == TOK_OP_NE) {
        char *op = arena_strdup(p->scratch_arena, p->current_lexeme);
        match(p, p->current_token);
        ASTNode *right = parse_relational(p);
        ASTNode *node = newNode(p->ast_arena, ND_BINOP, op);
        addChild(p->ast_arena, node, left);
        addChild(p->ast_arena, node, right);
        left = node;
    }
    return left;
}

static ASTNode *parse_relational(Parser *p) {
    ASTNode *left = parse_additive(p);
    while (p->current_token == TOK_OP_LT || p->current_token == TOK_OP_GT ||
           p->current_token == TOK_OP_LE || p->current_token == TOK_OP_GE) {
        char *op = arena_strdup(p->scratch_arena, p->current_lexeme);
        match(p, p->current_token);
        ASTNode *right = parse_additive(p);
        ASTNode *node = newNode(p->ast_arena, ND_BINOP, op);
        addChild(p->ast_arena, node, left);
        addChild(p->ast_arena, node, right);
        left = node;
    }
    return left;
}

static ASTNode *parse_additive(Parser *p) {
    ASTNode *left = parse_term(p);
    while (p->current_token == TOK_OP_PLUS || p->current_token == TOK_OP_MINUS) {
        char *op = arena_strdup(p->scratch_arena, p->current_lexeme);
        match(p, p->current_token);
        ASTNode *right = parse_term(p);
        ASTNode *node = newNode(p->ast_arena, ND_BINOP, op);
        addChild(p->ast_arena, node, left);
        addChild(p->ast_arena, node, right);
        left = node;
    }
    return left;
}

static ASTNode *parse_term(Parser *p) {
    ASTNode *left = parse_unary(p);
    while (p->current_token == TOK_OP_MUL || p->current_token == TOK_OP_DIV ||
           p->current_token == TOK_OP_MOD) {
        char *op = arena_strdup(p->scratch_arena, p->current_lexeme);
        match(p, p->current_token);
        ASTNode *right = parse_unary(p);
        ASTNode *node = newNode(p->ast_arena, ND_BINOP, op);
        addChild(p->ast_arena, node, left);
        addChild(p->ast_arena, node, right);
        left = node;
    }
    return left;
}

static ASTNode *parse_unary(Parser *p) {
    if (p->current_token == TOK_OP_NOT || p->current_token == TOK_OP_MINUS) {
        // Unary operator with right-associative binding (allows chaining: !!x or --x).
        char *op = arena_strdup(p->scratch_arena, p->current_lexeme);
        match(p, p->current_token);
        ASTNode *operand = parse_unary(p);
        ASTNode *node = newNode(p->ast_arena, ND_UNARY, op);
        addChild(p->ast_arena, node, operand);
        return node;
    }
    return parse_factor(p);
}

/* =========================================================================
 * Primary expressions: identifier, call, array access, literals, parenthesized
 * ========================================================================= */

/**
 * @brief Parses a primary expression.
 *
 * @param p Parser context.
 * @return AST node for the factor.
 */
static ASTNode *parse_factor(Parser *p) {
    switch (p->current_token) {
        case TOK_ID: {
            char *name = arena_strdup(p->scratch_arena, p->current_lexeme);
            match(p, TOK_ID);

            // Check what follows the identifier: function call, array access, or plain variable.
            if (p->current_token == TOK_DEL_LPAREN)
                return parse_call(p, name);
            else if (p->current_token == TOK_DEL_LBRACK)
                return parse_array_access(p, name);
            else
                return newNode(p->ast_arena, ND_ID, name);
        }

        case TOK_NUM_INT: {
            ASTNode *node = newNode(p->ast_arena, ND_NUM_INT, p->current_lexeme);
            match(p, TOK_NUM_INT);
            return node;
        }

        case TOK_NUM_FLOAT: {
            ASTNode *node = newNode(p->ast_arena, ND_NUM_FLOAT, p->current_lexeme);
            match(p, TOK_NUM_FLOAT);
            return node;
        }

        case TOK_DEL_LPAREN:
            return parse_parenthesized(p);

        default:
            // Unexpected token in an expression context.
            ec_report_cascading(lexer_current_line(),
                        "token inatteso %d in un'espressione", p->current_token);
            return newNode(p->ast_arena, ND_ERROR, "<error>");
    }
}
/**
 * @brief Parses a function call: 'name(expr, ...)'
 *
 * @param p    Parser context.
 * @param name Function name.
 * @return ND_CALL node.
 */
static ASTNode *parse_call(Parser *p, const char *name) {
    match(p, TOK_DEL_LPAREN);
    ASTNode *call_node = newNode(p->ast_arena, ND_CALL, name);

    // Parse zero or more arguments separated by commas.
    if (p->current_token != TOK_DEL_RPAREN) {
        addChild(p->ast_arena, call_node, parse_expr(p));
        while (p->current_token == TOK_DEL_COMMA) {
            match(p, TOK_DEL_COMMA);
            addChild(p->ast_arena, call_node, parse_expr(p));
        }
    }
    match(p, TOK_DEL_RPAREN);
    return call_node;
}

/**
 * @brief Parses an array access: 'name[expr]'
 *
 * @param p    Parser context.
 * @param name Array name.
 * @return ND_ARRAY_ACCESS node.
 */
static ASTNode *parse_array_access(Parser *p, const char *name) {
    match(p, TOK_DEL_LBRACK);
    ASTNode *index = parse_expr(p);
    match(p, TOK_DEL_RBRACK);

    ASTNode *access_node = newNode(p->ast_arena, ND_ARRAY_ACCESS, name);
    addChild(p->ast_arena, access_node, index);
    return access_node;
}

/**
 * @brief Parses a parenthesised expression: '(' expr ')'
 *
 * @param p Parser context.
 * @return AST node for the inner expression.
 */
static ASTNode *parse_parenthesized(Parser *p) {
    match(p, TOK_DEL_LPAREN);
    ASTNode *node = parse_expr(p);
    match(p, TOK_DEL_RPAREN);
    return node;
}