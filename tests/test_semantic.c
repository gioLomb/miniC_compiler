#include <stdio.h>
#include "lexer.h"
#include "parser/error.h"
#include "parser/ast.h"
#include "parser/parser.h"
#include "arena.h"
#include "symbol_table.h"
#include "ast_to_symtab.h"
#include "semantic.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <file_sorgente.c>\n", argv[0]);
        return 1;
    }

    lexer_open(argv[1]);
    Arena   *astArena = arena_create(0);
    ASTNode *root     = ParseProgram(astArena);
    lexer_close();

    printf("Parsing: %d errori.\n", totalErrorCount());

    Scope *global = scope_create(NULL);

    int pass1Errors = symtab_populate_globals(root, global);
    printf("Pass 1 (signature globali): %d errori.\n", pass1Errors);

    int semErrors = semantic_check(root, global);
    printf("Pass 2 + analisi semantica: %d errori.\n", semErrors);

    symtab_destroy_tree(global);
    freeAST(root);
    arena_destroy(astArena);

    return (pass1Errors > 0 || semErrors > 0) ? 1 : 0;
}