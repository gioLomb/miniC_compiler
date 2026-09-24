#ifndef TOKENS_H
#define TOKENS_H


/**
 * @file tokens.h
 * @brief Token type identifiers shared between the scanner and the parser.
 * Defines the enum of keyword, operator, delimiter, and literal token
 * codes produced by lexer_next_token().
 */

// Esempio di enum con i token necessari
enum {
    TOK_EOF = 0,
    TOK_ID,
    TOK_NUM_INT,
    TOK_NUM_FLOAT,
    TOK_KW_INT,
    TOK_KW_FLOAT,
    TOK_KW_IF,
    TOK_KW_ELSE,
    TOK_KW_WHILE,
    TOK_KW_RETURN,
    TOK_OP_ASSIGN,
    TOK_OP_PLUS,
    TOK_OP_MINUS,
    TOK_OP_MUL,
    TOK_OP_DIV,
    TOK_OP_MOD,    // Questo risolve l'errore del %
    TOK_OP_LT,
    TOK_OP_GT,
    TOK_OP_LE,
    TOK_OP_GE,
    TOK_OP_EQ,
    TOK_OP_NE,
    TOK_OP_AND,
    TOK_OP_OR,
    TOK_OP_NOT,    // Questo risolve l'errore del !
    TOK_DEL_SEMICOLON,
    TOK_DEL_LPAREN,
    TOK_DEL_RPAREN,
    TOK_DEL_LBRACK,
    TOK_DEL_RBRACK,
    TOK_DEL_LBRACE,
    TOK_DEL_RBRACE,
    TOK_DEL_COMMA,
    TOK_KW_FOR
};

/**
 * @brief Human-readable name for a token code (for parser error messages).
 */
static inline const char *tok_name(int tok) {
    switch (tok) {
    case TOK_EOF:          return "fine input";
    case TOK_ID:           return "identificatore";
    case TOK_NUM_INT:      return "numero intero";
    case TOK_NUM_FLOAT:    return "numero float";
    case TOK_KW_INT:       return "'int'";
    case TOK_KW_FLOAT:     return "'float'";
    case TOK_KW_IF:        return "'if'";
    case TOK_KW_ELSE:      return "'else'";
    case TOK_KW_WHILE:     return "'while'";
    case TOK_KW_RETURN:    return "'return'";
    case TOK_KW_FOR:       return "'for'";
    case TOK_OP_ASSIGN:    return "'='";
    case TOK_OP_PLUS:      return "'+'";
    case TOK_OP_MINUS:     return "'-'";
    case TOK_OP_MUL:       return "'*'";
    case TOK_OP_DIV:       return "'/'";
    case TOK_OP_MOD:       return "'%'";
    case TOK_OP_LT:        return "'<'";
    case TOK_OP_GT:        return "'>'";
    case TOK_OP_LE:        return "'<='";
    case TOK_OP_GE:        return "'>='";
    case TOK_OP_EQ:        return "'=='";
    case TOK_OP_NE:        return "'!='";
    case TOK_OP_AND:       return "'&&'";
    case TOK_OP_OR:        return "'||'";
    case TOK_OP_NOT:       return "'!'";
    case TOK_DEL_SEMICOLON:return "';'";
    case TOK_DEL_LPAREN:   return "'('";
    case TOK_DEL_RPAREN:   return "')'";
    case TOK_DEL_LBRACK:   return "'['";
    case TOK_DEL_RBRACK:   return "']'";
    case TOK_DEL_LBRACE:   return "'{'";
    case TOK_DEL_RBRACE:   return "'}'";
    case TOK_DEL_COMMA:    return "','";
    default:               return "token sconosciuto";
    }
}

#endif
