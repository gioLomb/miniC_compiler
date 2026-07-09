%{
#include <stdio.h>
/* Qui verranno inclusi i token generati dal Parser (es. yacc/bison) 
   Ad esempio: #include "y.tab.h" 
*/
%}

/* 0. DEFINIZIONI REGOLARI (MACRO) */
LETTERA     [a-zA-Z_]
CIFRA       [0-9]
WS          [ \t\n\r]

%%

"int"       { printf("TOKEN: KW_INT (%s)\n", yytext); return 1; }
"float"     { printf("TOKEN: KW_FLOAT (%s)\n", yytext); return 2; }
"if"        { printf("TOKEN: KW_IF (%s)\n", yytext); return 3; }
"else"      { printf("TOKEN: KW_ELSE (%s)\n", yytext); return 4; }
"while"     { printf("TOKEN: KW_WHILE (%s)\n", yytext); return 5; }
"for"       { printf("TOKEN: KW_FOR (%s)\n", yytext); return 6; }
"return"    { printf("TOKEN: KW_RETURN (%s)\n", yytext); return 7; }

{LETTERA}({LETTERA}|{CIFRA})* { printf("TOKEN: ID (%s)\n", yytext); return 8; }

{CIFRA}+                      { printf("TOKEN: NUM_INT (%s)\n", yytext); return 9; }
({CIFRA}+\.{CIFRA}*)|(\.{CIFRA}+) { printf("TOKEN: NUM_FLOAT (%s)\n", yytext); return 10; }

"+"         { printf("TOKEN: OP_PLUS (%s)\n", yytext); return 11; }
"-"         { printf("TOKEN: OP_MINUS (%s)\n", yytext); return 12; }
"*"         { printf("TOKEN: OP_MUL (%s)\n", yytext); return 13; }
"/"         { printf("TOKEN: OP_DIV (%s)\n", yytext); return 14; }
"%"         { printf("TOKEN: OP_MOD (%s)\n", yytext); return 15; }

"=="        { printf("TOKEN: OP_EQ (%s)\n", yytext); return 16; }
"!="        { printf("TOKEN: OP_NE (%s)\n", yytext); return 17; }
"<="        { printf("TOKEN: OP_LE (%s)\n", yytext); return 18; }
">="        { printf("TOKEN: OP_GE (%s)\n", yytext); return 19; }
"<"         { printf("TOKEN: OP_LT (%s)\n", yytext); return 20; }
">"         { printf("TOKEN: OP_GT (%s)\n", yytext); return 21; }

"&&"        { printf("TOKEN: OP_AND (%s)\n", yytext); return 22; }
"||"        { printf("TOKEN: OP_OR (%s)\n", yytext); return 23; }
"!"         { printf("TOKEN: OP_NOT (%s)\n", yytext); return 24; }
"="         { printf("TOKEN: OP_ASSIGN (%s)\n", yytext); return 25; }

"("         { printf("TOKEN: DEL_LPAREN (%s)\n", yytext); return 26; }
")"         { printf("TOKEN: DEL_RPAREN (%s)\n", yytext); return 27; }
"["         { printf("TOKEN: DEL_LBRACK (%s)\n", yytext); return 28; }
"]"         { printf("TOKEN: DEL_RBRACK (%s)\n", yytext); return 29; }
"{"         { printf("TOKEN: DEL_LBRACE (%s)\n", yytext); return 30; }
"}"         { printf("TOKEN: DEL_RBRACE (%s)\n", yytext); return 31; }
","         { printf("TOKEN: DEL_COMMA (%s)\n", yytext); return 32; }
";"         { printf("TOKEN: DEL_SEMICOLON (%s)\n", yytext); return 33; }


"//".* { /* Ignora i commenti a linea singola */ }
"/*"([^*]|\*+[^*/])*\*+"/" { /* Ignora i commenti multilinea stile C */ }
[ \t\n\r]+              { /* Ignora spazi, tab e rinvii a capo */ }

.           { printf("ERRORE LESSICALE: Carattere non valido '%s'\n", yytext); }

%%

int yywrap() {
    return 1;
}

int main(int argc, char **argv) {
    if (argc > 1) {
        FILE *file = fopen(argv[1], "r");
        if (!file) {
            fprintf(stderr, "Impossibile aprire il file %s\n", argv[1]);
            return 1;
        }
        yyin = file;
    }
    while(yylex());
    return 0;
}