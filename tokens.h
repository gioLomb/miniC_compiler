
#ifndef TOKENS_H
#define TOKENS_H

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

#endif
