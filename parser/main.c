#include <stdio.h>
#include "../lexer.h"
#include "error.h"
#include "ast.h"
#include "parser.h"
#include "../symbol_table.h"
#include "../ast_to_symtab.h"
#include "../semantic.h"
#include "../optimize.h"
#include "../ir.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <file_sorgente.c>\n", argv[0]);
        return 1;
    }

    lexer_open(argv[1]);
    ASTNode *root = ParseProgram();
    lexer_close();

    printf("=== PARSE TREE ===\n");
    printAST(root, 0);

    int parseErrors = totalErrorCount();
    if (parseErrors > 0) {
        printf("\nParsing completato con %d error%s.\n", parseErrors, parseErrors == 1 ? "e" : "i");
        freeAST(root);
        return 1;
    }
    printf("\nParsing completato con successo.\n");

    Scope *global = scope_create(NULL);
    int pass1Errors = symtab_populate_globals(root, global);
    int semErrors = semantic_check(root, global);
    printf("\n=== ANALISI SEMANTICA ===\n");
    printf("Pass 1 (signature globali): %d error%s.\n", pass1Errors, pass1Errors == 1 ? "e" : "i");
    printf("Pass 2 + analisi semantica: %d error%s.\n", semErrors, semErrors == 1 ? "e" : "i");

    int totalErrors = pass1Errors + semErrors;
    if (totalErrors == 0) {
        optimize_ast(root);

        IRProgram *ir = ir_generate(root);
        printf("\n=== IR LINEARE (three-address code) ===\n");
        ir_print(ir);
        ir_free(ir);
    } else {
        printf("\nErrori presenti: l'IR non viene generato.\n");
    }

    symtab_destroy_tree(global);
    freeAST(root);

    return totalErrors > 0 ? 1 : 0;
}