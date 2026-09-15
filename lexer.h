#ifndef LEXER_H
#define LEXER_H


/**
 * @file lexer.h
 * @brief Lexical analyzer interface for reading and tokenizing source files.
 *
 * Opens a source file, maintains an internal character buffer and yields
 * successive tokens (keywords, identifiers, literals, operators) together
 * with their source locations for the recursive-descent parser.
 */

/**
 * @brief Opens the source file and loads its entire contents into an internal buffer.
 *
 * Terminates the program with an error message if the file cannot be opened.
 *
 * @param path Path to the source file to open.
 */
void lexer_open(const char *path);

/**
 * @brief Frees the buffer memory allocated by `lexer_open`.
 */
void lexer_close(void);

/**
 * @brief Retrieves the token code for the next token in the input stream.
 *
 * @return The token code integer (`TOK_EOF` when end of input is reached).
 */
int lexer_next_token(void);

/**
 * @brief Returns the text of the last token returned by `lexer_next_token()`.
 *
 * @note The returned pointer remains valid only until the next call to `lexer_next_token()`.
 *
 * @return Pointer to the current lexeme string.
 */
const char *lexer_current_lexeme(void);

/**
 * @brief Retrieves the current line number (used for error reporting).
 *
 * @return Current 1-based line number.
 */
int lexer_current_line(void);

#endif /* LEXER_H */
