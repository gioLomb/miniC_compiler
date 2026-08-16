#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#define ASCII_CHAR_LEN 256
#define NUM_STATES 6
#define NUM_CLASSES 5
#define STATE_ERROR -1
#define STATE_START 0
#define MAX_INPUT_LEN 1024
// MiniC token type
typedef enum {
    TOKEN_ERROR = -1,
    TOKEN_NONE = 0,
    TOKEN_ID,
    TOKEN_NUM_INT,
    TOKEN_OP_ASSIGN,
    TOKEN_OP_EQ,
    TOKEN_WHITESPACE
} TokenType;

typedef enum {
    CL_LETTER,
    CL_DIGIT,
    CL_EQUALS,
    CL_WHITESPACE,
    CL_OTHER
} charClass;

typedef struct {
    const char* source;    // entire source
    int lexeme_start;      // current lexeme start
} ScannerInput;

unsigned char charMap[ASCII_CHAR_LEN];
int TransitionTable[NUM_STATES][NUM_CLASSES];
TokenType TokenTable[NUM_STATES];

/* Global counter for input stream position */
int InputPos = 0;

/* Bit-array to memoize dead-end transitions (ref: EAC Fig. 2.13) */
bool Failed[NUM_STATES][MAX_INPUT_LEN];

void init_char_map(){
    memset(charMap,CL_OTHER,ASCII_CHAR_LEN);

    memset(charMap+'a',CL_LETTER,'z'-'a'+1);
    memset(charMap+'A',CL_LETTER,'Z'-'A'+1);
    charMap['_'] = CL_LETTER;

    memset(charMap+'0',CL_DIGIT,'9'-'0');

    charMap['='] = CL_EQUALS;
    
    charMap[' ']  = CL_WHITESPACE;
    charMap['\t'] = CL_WHITESPACE;
    charMap['\n'] = CL_WHITESPACE;
    charMap['\r'] = CL_WHITESPACE;
}

void init_dfa_tables() {

    memset(TransitionTable, -1, sizeof(TransitionTable));
    
    memset(TokenTable, 0, sizeof(TokenTable));

    TokenTable[1] = TOKEN_ID;
    TokenTable[2] = TOKEN_NUM_INT;
    TokenTable[3] = TOKEN_OP_ASSIGN;
    TokenTable[4] = TOKEN_OP_EQ;
    TokenTable[5] = TOKEN_WHITESPACE;

    TransitionTable[STATE_START][CL_LETTER]     = 1;
    TransitionTable[STATE_START][CL_DIGIT]      = 2;
    TransitionTable[STATE_START][CL_EQUALS]     = 3;
    TransitionTable[STATE_START][CL_WHITESPACE] = 5;
    TransitionTable[1][CL_LETTER] = 1;
    TransitionTable[1][CL_DIGIT]  = 1;
    TransitionTable[2][CL_DIGIT]  = 2;
    TransitionTable[3][CL_EQUALS] = 4;
    TransitionTable[5][CL_WHITESPACE] = 5;
}


/* Global initialization of the scanner system */
void init_scanner_system() {
    init_char_map();
    init_dfa_tables();
    InputPos = 0;
    memset(Failed, false, sizeof(Failed));
}

/* 
 * next_token: Implements the Maximal Munch scanner with memoized rollback.
 * Uses an explicit stack to keep track of DFA states and input positions.
 */
TokenType next_token(const char* source, char* output_lexeme) {
    int state = STATE_START;
    int state_stack[MAX_INPUT_LEN];
    int pos_stack[MAX_INPUT_LEN];
    int stack_ptr = 0;

    int lexeme_start_pos = InputPos;

    if (source[InputPos] == '\0') return TOKEN_NONE;

    /* Main exploration loop: Move forward until a dead-end or EOF */
    while (state != STATE_ERROR && source[InputPos] != '\0') {
        /* Memoization check: Avoid re-exploring known dead-end paths */
        if (Failed[state][InputPos]) break; 

        state_stack[stack_ptr] = state;
        pos_stack[stack_ptr] = InputPos;
        stack_ptr++;

        unsigned char c = (unsigned char)source[InputPos];
        unsigned char char_class = charMap[c];
        state = TransitionTable[state][char_class];

        if (state != STATE_ERROR) InputPos++;
    }

    if (state != STATE_ERROR) {
        state_stack[stack_ptr] = state;
        pos_stack[stack_ptr] = InputPos;
        stack_ptr++;
    }

    /* Rollback phase: Unwind stack until an accepting state is found */
    while (stack_ptr > 0) {
        stack_ptr--;
        int current_state = state_stack[stack_ptr];
        int current_pos = pos_stack[stack_ptr];

        if (TokenTable[current_state] != TOKEN_NONE) {
            int len = InputPos - lexeme_start_pos;
            strncpy(output_lexeme, &source[lexeme_start_pos], len);
            output_lexeme[len] = '\0';
            return TokenTable[current_state];
        }

        /* Memoize the dead-end to optimize future scanner calls */
        Failed[current_state][current_pos] = true;
        InputPos = current_pos; 
    }

    /* Lexical error handling: Skip erroneous character and report */
    output_lexeme[0] = source[lexeme_start_pos];
    output_lexeme[1] = '\0';
    InputPos = lexeme_start_pos + 1;
    return TOKEN_ERROR;
}

int main() {
    init_scanner_system();
    const char* code = "x = 10 == y";
    char lexeme[256];
    TokenType token;

    printf("Executing EAC Fig. 2.13 Maximal Munch Scanner...\n\n");

    while ((token = next_token(code, lexeme)) != TOKEN_NONE) {
        if (token == TOKEN_ERROR) printf("[LEXICAL ERROR]\n");
        else printf("TOKEN: %d | Lexeme: '%s' | Next InputPos: %d\n", token, lexeme, InputPos);
    }

    return 0;
}
