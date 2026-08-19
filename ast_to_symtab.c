#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast_to_symtab.h"

DataType st_resolve_type(const char *typeName) {
    if (!typeName) return T_VOID;

    switch (typeName[0]) {
    case 'i': return T_INT;
    case 'f': return T_FLOAT;
    default:  return T_VOID;
    }
}

void st_elaborate_decl(Arena *arena, const char *text,
                             char **outTypeName, char **outName,
                             int *isArray, int *arraySize) {
    *isArray = 0;
    *arraySize = 0;
    *outTypeName = NULL;
    *outName = NULL;
    if (!text) return;

    char *buf = arena_strdup(arena, text);

    char *space = strchr(buf, ' ');
    if (!space) {
        *outTypeName = buf;
        return;
    }
    *space = '\0';
    *outTypeName = buf;

    char *rest = space + 1;
    char *bracket = strchr(rest, '[');
    if (bracket) {
        *bracket = '\0';
        *outName = rest;
        *isArray = 1;
        *arraySize = atoi(bracket + 1);
    } else {
        *outName = rest;
    }
}

int st_bind_symbol(Arena *arena, Scope *scope, ASTNode *node) {
    char *typeName, *name;
    int isArray, arraySize;

    st_elaborate_decl(arena, node->text, &typeName, &name, &isArray, &arraySize);

    Symbol sym = {
        .kind       = SYM_VAR,
        .dataType   = st_resolve_type(typeName),
        .isArray    = isArray,
        .arraySize  = arraySize,
        .scopeLevel = scope->level,
        .offset     = (int)scope->table->size
    };

    if (!sym_bind(scope, name, &sym)) {
        fprintf(stderr, "Errore: '%s' e' gia' stato dichiarato in questo scope\n", name);
        return 0;
    }

    node->scopeLevel = sym.scopeLevel;
    node->offset     = sym.offset;
    return 1;
}

int st_resolve_global_namespace(ASTNode *program, Scope *global) {
    int errors = 0;

    Arena *arena = arena_create(0);

    for (int i = 0; i < program->nchildren; i++) {
        ASTNode *decl = program->children[i];

        char *typeName, *name;
        int isArray, arraySize;
        st_elaborate_decl(arena, decl->text, &typeName, &name, &isArray, &arraySize);

        Symbol sym = {0};
        sym.dataType = st_resolve_type(typeName);

        if (decl->kind == ND_FUNC_DECL) {
            sym.kind = SYM_FUNC;

            int paramCount = decl->nchildren - 1;
            if (paramCount > SYM_MAX_PARAMS) {
                fprintf(stderr,
                        "Errore: '%s' ha %d parametri, il massimo supportato e' %d\n",
                        name, paramCount, SYM_MAX_PARAMS);
                paramCount = SYM_MAX_PARAMS;
                errors++;
            }
            sym.paramCount = paramCount;

            for (int p = 0; p < paramCount; p++) {
                char *ptypeName, *pname;
                int pIsArray, pArraySize;
                st_elaborate_decl(arena, decl->children[p]->text,
                                        &ptypeName, &pname, &pIsArray, &pArraySize);

                DataType pType = st_resolve_type(ptypeName);
                symtab_pack_param_type(&sym.paramTypes, p, pType);
            }

        } else if (decl->kind == ND_VAR_DECL) {
            sym.kind      = SYM_VAR;
            sym.isArray   = isArray;
            sym.arraySize = arraySize;

        } else {
            continue;
        }

        /* FIX: ogni simbolo globale (var o func) riceve offset sequenziale unico
         * basato su quante entry sono già nella tabella hash globale.
         * Senza questo fix tutti i globali collassavano a offset 0 perché
         * sym.offset non veniva mai impostato prima del sym_bind. */
        sym.scopeLevel = global->level;            /* sempre 0 */
        sym.offset     = (int)global->table->size; /* prossimo slot libero */

        if (!sym_bind(global, name, &sym)) {
            fprintf(stderr, "Errore: '%s' e' gia' stato dichiarato in questo scope\n", name);
            errors++;
        } else {
            /* Stampa offset su AST node per ND_VAR_DECL globale
             * (usato da ir_add_global per abbinare symOffset) */
            if (decl->kind == ND_VAR_DECL) {
                decl->scopeLevel = sym.scopeLevel;
                decl->offset     = sym.offset;
            }
        }
    }

    arena_destroy(arena);
    return errors;
}