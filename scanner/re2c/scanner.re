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
#include "../../arena.h"


/* --- "yytext" equivalente: cur punta al carattere corrente, tok
   all'inizio del lessema in corso di riconoscimento ---
   'cur' e 'tok' sono STATIC: interni a questo file. Il resto del
   programma (parser.c) non li vede mai direttamente: puo' solo usare
   le funzioni lexer_open/lexer_next_token/lexer_current_lexeme/
   lexer_current_line dichiarate in lexer.h. Questo evita gli 'extern'
   che attraversavano il confine tra modulo scanner e modulo parser. */
static const unsigned char *cur;
static const unsigned char *tok;
static int lineNumber = 1;
static unsigned char *sourceBuffer = NULL;   /* buffer allocato da lexer_open */

#define TOKLEN   ((int)(cur - tok))
#define TOKTEXT  ((const char *)tok)

/*
 * yylex() - equivalente della funzione generata da Flex.
 * Ogni iterazione del for(;;) riconosce un token; i token da ignorare
 * (whitespace, commenti) fanno "continue" invece di "return".
 */
static int yylex(void) {
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

            "int"     { return TOK_KW_INT; }
            "float"   { return TOK_KW_FLOAT; }
            "if"      { return TOK_KW_IF; }
            "else"    { return TOK_KW_ELSE; }
            "while"   { return TOK_KW_WHILE; }
            "for"     { return TOK_KW_FOR; }
            "return"  { return TOK_KW_RETURN; }

            id        { return TOK_ID; }

            float_lit { return TOK_NUM_FLOAT; }
            int_lit   { return TOK_NUM_INT; }

            "+"       { return TOK_OP_PLUS; }
            "-"       { return TOK_OP_MINUS; }
            "*"       { return TOK_OP_MUL; }
            "/"       { return TOK_OP_DIV; }
            "%"       { return TOK_OP_MOD; }

            "=="      { return TOK_OP_EQ; }
            "!="      { return TOK_OP_NE; }
            "<="      { return TOK_OP_LE; }
            ">="      { return TOK_OP_GE; }
            "<"       { return TOK_OP_LT; }
            ">"       { return TOK_OP_GT; }

            "&&"      { return TOK_OP_AND; }
            "||"      { return TOK_OP_OR; }
            "!"       { return TOK_OP_NOT; }
            "="       { return TOK_OP_ASSIGN; }

            "("       { return TOK_DEL_LPAREN; }
            ")"       { return TOK_DEL_RPAREN; }
            "["       { return TOK_DEL_LBRACK; }
            "]"       { return TOK_DEL_RBRACK; }
            "{"       { return TOK_DEL_LBRACE; }
            "}"       { return TOK_DEL_RBRACE; }
            ","       { return TOK_DEL_COMMA; }
            ";"       { return TOK_DEL_SEMICOLON; }

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

/* ==================================================================
 * API PUBBLICA DEL LEXER (lexer.h) - unico punto di contatto con il
 * resto del programma. Nessuno fuori da questo file vede piu'
 * 'cur'/'tok'/'yylex' direttamente: niente extern nel parser.
 * ================================================================== */

#include "../../lexer.h"

/* Arena del lexer: possiede il testo di ogni lessema restituito da
   lexer_current_lexeme(). Nessun limite di lunghezza fisso (a
   differenza del vecchio "char lexemeBuf[LEXEME_MAX]"): tok/cur
   puntano gia' dentro sourceBuffer (l'intero file, caricato una
   volta da lexer_open), quindi la dimensione totale mai occupata
   da questa arena e' comunque limitata dalla dimensione del file
   sorgente stesso - non serve un cap ad hoc separato. */
static Arena *lexerArena = NULL;
static char *currentLexeme = NULL;

void lexer_open(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Impossibile aprire il file %s\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    lexerArena = arena_create(0);                     // crea l'arena

    unsigned char *buf = arena_alloc(lexerArena, len + 1); // alloca direttamente nell'arena
    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);

    cur = buf;                                         // punta all'inizio del sorgente
    lineNumber = 1;
}

void lexer_close(void) {
    arena_destroy(lexerArena);    // libera tutto (sourceBuffer + lessemi)
    lexerArena = NULL;
    currentLexeme = NULL;
    cur = NULL;
}

int lexer_next_token(void) {
    int token = yylex();

    int len = (int)(cur - tok);
    if (len < 0) len = 0;
    currentLexeme = arena_strndup(lexerArena, (const char *)tok, (size_t)len);

    return token;
}

const char *lexer_current_lexeme(void) {
    return currentLexeme;
}

int lexer_current_line(void) {
    return lineNumber;
}
