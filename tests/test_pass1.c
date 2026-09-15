#include <stdio.h>
#include "lexer.h"
#include "parser/errorCollector.h"
#include "parser/ast.h"
#include "parser/parser.h"
#include "arena.h"
#include "symbol_table.h"
#include "ast_to_symtab.h"

static const char *dataTypeName(DataType t) {
    switch (t) {
        case T_INT:   return "int";
        case T_FLOAT: return "float";
        case T_VOID:  return "void";
    }
    return "?";
}

static void printSymbol(void *key, size_t keySize, void *value, size_t valueSize, void *userdata) {
    (void)valueSize; (void)userdata;
    const char *name = (const char *)key;
    int nameLen = (int)keySize;
    Symbol *sym = (Symbol *)value;
    if (sym->kind == SYM_FUNC) {
        printf("  FUNC %.*s -> %s (", nameLen, name, dataTypeName(sym->dataType));
        for (int i = 0; i < sym->paramCount; i++) {
            if (i > 0) printf(", ");
            printf("%s", dataTypeName(symtab_unpack_param_type(sym->paramTypes, i)));
        }
        printf(")\n");
    } else {
        printf("  VAR  %.*s : %s%s\n", nameLen, name, dataTypeName(sym->dataType),
               sym->isArray ? " (array)" : "");
    }
}

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
    int symErrors = st_resolve_global_namespace(root, global);
    printf("Pass 1 (global scope population): %d errors.\n\n", symErrors);

    printf("=== GLOBAL SCOPE ===\n");
    ht_foreach(global->table, printSymbol, NULL);

    sym_finalize(global);
    arena_destroy(astArena);

    return symErrors > 0 ? 1 : 0;
}
