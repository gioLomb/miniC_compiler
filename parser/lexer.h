#ifndef LEXER_H
#define LEXER_H

/* Apre il file sorgente e lo carica interamente in un buffer interno.
   Termina il programma con un messaggio se il file non esiste. */
void lexer_open(const char *path);

/* Libera il buffer allocato da lexer_open. */
void lexer_close(void);

/* Restituisce il codice del prossimo token (TOK_EOF a fine input). */
int lexer_next_token(void);

/* Restituisce il testo dell'ultimo token restituito da lexer_next_token().
   Il puntatore resta valido solo fino alla chiamata successiva. */
const char *lexer_current_lexeme(void);

/* Restituisce il numero di riga corrente (per i messaggi di errore). */
int lexer_current_line(void);

#endif
