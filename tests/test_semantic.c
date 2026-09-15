#include <stdio.h>
#include "lexer.h"
#include "parser/errorCollector.h"
#include "parser/ast.h"
#include "parser/parser.h"
#include "arena.h"
#include "symbol_table.h"
#include "ast_to_symtab.h"
#include "semantic.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <source_file.c>\n", argv[0]);
        return 1;
    }

    lexer_open(argv[1]);
    Arena   *astArena = arena_create(0);
    ASTNode *root     = ParseProgram(astArena);
    lexer_close();

    printf("Parsing: %d errors.\n", ec_error_count());

    Scope *global = sym_scopeCreate(NULL);

    int pass1Errors = st_resolve_global_namespace(root, global);
    printf("Pass 1 (global signatures): %d errors.\n", pass1Errors);

    int semErrors = semantic_check(root, global);
    printf("Pass 2 + semantic analysis: %d errors.\n", semErrors);

    sym_finalize(global);
    arena_destroy(astArena);

    return (pass1Errors > 0 || semErrors > 0) ? 1 : 0;
}
