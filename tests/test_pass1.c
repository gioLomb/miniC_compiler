#include <stdio.h>
#include "lexer.h"
#include "parser/error.h"
#include "parser/ast.h"
#include "parser/parser.h"
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

/* userdata per ht_foreach: stampa ogni Symbol registrato nello scope globale */
static void printSymbol(void *key, size_t keySize, void *value, size_t valueSize, void *userdata) {
    (void)keySize; (void)valueSize; (void)userdata;
    char name[SYM_MAX_NAME_LEN];
    snprintf(name, sizeof(name), "%.*s", (int)keySize, (char *)key);

    Symbol *sym = (Symbol *)value;
    if (sym->kind == SYM_FUNC) {
        printf("  FUNC %s -> %s (", name, dataTypeName(sym->dataType));
        for (int i = 0; i < sym->paramCount; i++) {
            if (i > 0) printf(", ");
            printf("%s", dataTypeName(symtab_unpack_param_type(sym->paramTypes, i)));
        }
        printf(")\n");
    } else {
        printf("  VAR  %s : %s%s\n", name, dataTypeName(sym->dataType),
               sym->isArray ? " (array)" : "");
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <file_sorgente.c>\n", argv[0]);
        return 1;
    }

    lexer_open(argv[1]);
    ASTNode *root = ParseProgram();
    printf("Parsing: %d errori.\n", totalErrorCount());

    Scope *global = scope_create(NULL);
    int symErrors = symtab_populate_globals(root, global);
    printf("Pass 1 (popolamento scope globale): %d errori.\n\n", symErrors);

    printf("=== SCOPE GLOBALE ===\n");
    ht_foreach(global->table, printSymbol, NULL);

    symtab_destroy_tree(global);
    freeAST(root);
    lexer_close();

    return symErrors > 0 ? 1 : 0;
}
