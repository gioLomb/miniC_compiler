/*
 * scanner.re - Analizzatore lessicale con re2c
 *
 * Equivalente del file scanner.l per Flex fornito: stesse regole,
 * stessi codici di token, stesso comportamento (stampa il token
 * riconosciuto e lo restituisce; ignora commenti/whitespace; segnala
 * errori lessicali).
 *
 * Differenza fondamentale rispetto a Flex: re2c NON genera un programma
 * completo. Genera solo l'istruzione di match (uno switch/goto ottimizzato)
 * a partire dal blocco speciale re2c inserito piu' sotto in yylex(), che va
 * incorporato in un ciclo di scansione scritto a mano: gestione del buffer,
 * di yytext equivalente (qui 'tok'/'cur'), di fine-file e il main sono
 * tutti a carico nostro.
 *
 * Compilazione:
 *   re2c scanner.re -o scanner.c
 *   gcc -Wall -o scanner scanner.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../tokens.h"


/* --- "yytext" equivalente: cur punta al carattere corrente, tok
   all'inizio del lessema in corso di riconoscimento --- */
/* 'cur' e 'tok' NON sono static: sono variabili globali condivise con
   parser.c. 'cur' e' inizializzata da parser.c prima di chiamare yylex();
   'tok'/'cur' insieme delimitano il lessema dell'ultimo token restituito,
   e parser.c le legge subito dopo ogni chiamata a yylex() per catturare
   il testo del token (vedi advance()/setLexeme() in parser.c). */
const unsigned char *cur;
const unsigned char *tok;
static int lineNumber = 1;

#define TOKLEN   ((int)(cur - tok))
#define TOKTEXT  ((const char *)tok)

/*
 * yylex() - equivalente della funzione generata da Flex.
 * Ogni iterazione del for(;;) riconosce un token; i token da ignorare
 * (whitespace, commenti) fanno "continue" invece di "return".
 */
int yylex(void) {
    const unsigned char *YYMARKER;   /* richiesto da re2c per il backtracking
                                         tra regole con prefissi comuni, es.
                                         "<" vs "<=" */

    for (;;) {
        tok = cur;

        /*!re2c
            re2c:define:YYCTYPE  = "unsigned char";
            re2c:define:YYCURSOR = cur;
            re2c:define:YYMARKER = YYMARKER;
            re2c:yyfill:enable   = 0;

            end       = "\x00";
            digit     = [0-9];
            letter    = [a-zA-Z_];
            id        = letter (letter|digit)*;
            int_lit   = digit+;
            float_lit = digit+ "." digit+;
            ws        = [ \t\r]+;
            newline   = "\n";
            comment   = "//" [^\n\x00]*;
            /* commento a blocco: /* ... * / (nessun "* /" al suo interno) */
            blockcomment = "/*" ([^*\x00] | ("*"+ [^*/\x00]))* "*"+ "/";

            "int"     { printf("TOKEN: KW_INT (%.*s)\n", TOKLEN, TOKTEXT); return TOK_KW_INT; }
            "float"   { printf("TOKEN: KW_FLOAT (%.*s)\n", TOKLEN, TOKTEXT); return TOK_KW_FLOAT; }
            "if"      { printf("TOKEN: KW_IF (%.*s)\n", TOKLEN, TOKTEXT); return TOK_KW_IF; }
            "else"    { printf("TOKEN: KW_ELSE (%.*s)\n", TOKLEN, TOKTEXT); return TOK_KW_ELSE; }
            "while"   { printf("TOKEN: KW_WHILE (%.*s)\n", TOKLEN, TOKTEXT); return TOK_KW_WHILE; }
            "for"     { printf("TOKEN: KW_FOR (%.*s)\n", TOKLEN, TOKTEXT); return TOK_KW_FOR; }
            "return"  { printf("TOKEN: KW_RETURN (%.*s)\n", TOKLEN, TOKTEXT); return TOK_KW_RETURN; }

            id        { printf("TOKEN: ID (%.*s)\n", TOKLEN, TOKTEXT); return TOK_ID; }

            float_lit { printf("TOKEN: NUM_FLOAT (%.*s)\n", TOKLEN, TOKTEXT); return TOK_NUM_FLOAT; }
            int_lit   { printf("TOKEN: NUM_INT (%.*s)\n", TOKLEN, TOKTEXT); return TOK_NUM_INT; }

            "+"       { printf("TOKEN: OP_PLUS (%.*s)\n", TOKLEN, TOKTEXT); return TOK_OP_PLUS; }
            "-"       { printf("TOKEN: OP_MINUS (%.*s)\n", TOKLEN, TOKTEXT); return TOK_OP_MINUS; }
            "*"       { printf("TOKEN: OP_MUL (%.*s)\n", TOKLEN, TOKTEXT); return TOK_OP_MUL; }
            "/"       { printf("TOKEN: OP_DIV (%.*s)\n", TOKLEN, TOKTEXT); return TOK_OP_DIV; }
            "%"       { printf("TOKEN: OP_MOD (%.*s)\n", TOKLEN, TOKTEXT); return TOK_OP_MOD; }

            "=="      { printf("TOKEN: OP_EQ (%.*s)\n", TOKLEN, TOKTEXT); return TOK_OP_EQ; }
            "!="      { printf("TOKEN: OP_NE (%.*s)\n", TOKLEN, TOKTEXT); return TOK_OP_NE; }
            "<="      { printf("TOKEN: OP_LE (%.*s)\n", TOKLEN, TOKTEXT); return TOK_OP_LE; }
            ">="      { printf("TOKEN: OP_GE (%.*s)\n", TOKLEN, TOKTEXT); return TOK_OP_GE; }
            "<"       { printf("TOKEN: OP_LT (%.*s)\n", TOKLEN, TOKTEXT); return TOK_OP_LT; }
            ">"       { printf("TOKEN: OP_GT (%.*s)\n", TOKLEN, TOKTEXT); return TOK_OP_GT; }

            "&&"      { printf("TOKEN: OP_AND (%.*s)\n", TOKLEN, TOKTEXT); return TOK_OP_AND; }
            "||"      { printf("TOKEN: OP_OR (%.*s)\n", TOKLEN, TOKTEXT); return TOK_OP_OR; }
            "!"       { printf("TOKEN: OP_NOT (%.*s)\n", TOKLEN, TOKTEXT); return TOK_OP_NOT; }
            "="       { printf("TOKEN: OP_ASSIGN (%.*s)\n", TOKLEN, TOKTEXT); return TOK_OP_ASSIGN; }

            "("       { printf("TOKEN: DEL_LPAREN (%.*s)\n", TOKLEN, TOKTEXT); return TOK_DEL_LPAREN; }
            ")"       { printf("TOKEN: DEL_RPAREN (%.*s)\n", TOKLEN, TOKTEXT); return TOK_DEL_RPAREN; }
            "["       { printf("TOKEN: DEL_LBRACK (%.*s)\n", TOKLEN, TOKTEXT); return TOK_DEL_LBRACK; }
            "]"       { printf("TOKEN: DEL_RBRACK (%.*s)\n", TOKLEN, TOKTEXT); return TOK_DEL_RBRACK; }
            "{"       { printf("TOKEN: DEL_LBRACE (%.*s)\n", TOKLEN, TOKTEXT); return TOK_DEL_LBRACE; }
            "}"       { printf("TOKEN: DEL_RBRACE (%.*s)\n", TOKLEN, TOKTEXT); return TOK_DEL_RBRACE; }
            ","       { printf("TOKEN: DEL_COMMA (%.*s)\n", TOKLEN, TOKTEXT); return TOK_DEL_COMMA; }
            ";"       { printf("TOKEN: DEL_SEMICOLON (%.*s)\n", TOKLEN, TOKTEXT); return TOK_DEL_SEMICOLON; }

            comment      { continue; }          /* commenti su singola riga: ignora */
            blockcomment {
                              /* un commento a blocco puo' contenere newline:
                                 aggiorna lineNumber per non perdere il conteggio righe */
                              for (const unsigned char *p = tok; p < cur; p++) {
                                  if (*p == '\n') lineNumber++;
                              }
                              continue;
                          }                     /* commenti a blocco /* ... * /: ignora */
            newline   { lineNumber++; continue; } /* newline: conta la riga e ignora */
            ws        { continue; }              /* spazi/tab/CR: ignora */

            end       { return 0; }              /* fine input */

            *         {
                          fprintf(stderr, "ERRORE LESSICALE (linea %d): carattere non valido '%c'\n",
                                  lineNumber, tok[0]);
                          continue;
                      }
        */
    }
}

/* --- main: legge l'intero file in un buffer null-terminated e scansiona --- */

