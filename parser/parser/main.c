#include <stdio.h>
#include "../lexer.h"
#include "error.h"
#include "ast.h"
#include "parser.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <file_sorgente.c>\n", argv[0]);
        return 1;
    }

    lexer_open(argv[1]);

    ASTNode *root = ParseProgram();

    printf("=== PARSE TREE ===\n");
    printAST(root, 0);

    int errors = totalErrorCount();
    if (errors > 0) {
        printf("\nParsing completato con %d error%s.\n", errors, errors == 1 ? "e" : "i");
    } else {
        printf("\nParsing completato con successo.\n");
    }

    freeAST(root);
    lexer_close();

    return errors > 0 ? 1 : 0;
}
