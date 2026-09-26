#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include "ast_to_symtab.h"
#include "parser/errorCollector.h"

DataType st_resolve_type(const char *type_name) {
    if (!type_name) return T_VOID;

    switch (type_name[0]) {
    case 'i': return T_INT;
    case 'f': return T_FLOAT;
    default:  return T_VOID;
    }
}


void st_elaborate_decl(Arena *arena, const char *text,
                       char **outTypeName, char **outName,
                       int *isArray, int *arraySize) {
    /* Kept for source compatibility / debug; hot path uses ASTNode.ident. */
    *isArray   = 0;
    *arraySize = 0;
    *outTypeName = NULL;
    *outName     = NULL;
    if (!text) return;

    char *buf = arena_strdup(arena, text);

    char *space = strchr(buf, ' ');
    if (!space) {
        *outTypeName = buf;
        return;
    }
    *space       = '\0';
    *outTypeName = buf;

    char *rest    = space + 1;
    char *bracket = strchr(rest, '[');
    if (bracket) {
        *bracket   = '\0';
        *outName   = rest;
        *isArray   = 1;
        char *end = NULL;
        errno = 0;
        long v = strtol(bracket + 1, &end, 10);
        if (end == bracket + 1 || (*end != ']' && *end != '\0') ||
            errno == ERANGE || v <= 0 || v > INT_MAX / 8) {
            *arraySize = 0;
        } else {
            *arraySize = (int)v;
        }
    } else {
        *outName = rest;
    }
}


int st_bind_symbol(Arena *arena, Scope *scope, ASTNode *node) {
    (void)arena;
    const char *name = node->ident;
    if (!name) return 0;

    if (node->isArray && node->arraySize <= 0) {
        ec_report("Errore: dimensione array non valida per '%s'\n", name);
        return 0;
    }

    Symbol sym = {
        .kind       = SYM_VAR,
        .dataType   = node->dataType,
        .isArray    = node->isArray,
        .arraySize  = node->arraySize,
        .scopeLevel = sym_scope_level(scope),
        .offset     = sym_scope_count(scope),
    };

    if (!sym_bind(scope, name, &sym)) {
        ec_report("Errore: '%s' e' gia' stato dichiarato in questo scope\n", name);
        return 0;
    }

    node->scopeLevel = sym.scopeLevel;
    node->offset     = sym.offset;
    node->dataType   = sym.dataType;
    return 1;
}

static int st_process_func_decl(ASTNode *decl, Symbol *sym, const char *name) {
    int errors = 0;
    sym->kind = SYM_FUNC;

    int paramCount = decl->nchildren - 1;
    if (paramCount > SYM_MAX_PARAMS) {
        ec_report("Errore: '%s' ha %d parametri, il massimo supportato e' %d\n",
                name, paramCount, SYM_MAX_PARAMS);
        paramCount = SYM_MAX_PARAMS;
        errors++;
    }
    sym->paramCount = paramCount;

    for (int p = 0; p < paramCount; p++)
        symtab_pack_param_type(&sym->paramTypes, p, decl->children[p]->dataType);

    return errors;
}

static inline void st_init_var_symbol(Symbol *sym, int isArray, int arraySize) {
    sym->kind      = SYM_VAR;
    sym->isArray   = isArray;
    sym->arraySize = arraySize;
}

static inline int st_bind_global_symbol(Scope *global, const char *name, Symbol *sym) {
    sym->scopeLevel = sym_scope_level(global);
    sym->offset     = sym_scope_count(global);

    if (!sym_bind(global, name, sym)) {
        ec_report("Errore: '%s' e' gia' stato dichiarato in questo scope\n", name);
        return 1;
    }
    return 0;
}


int st_resolve_global_namespace(ASTNode *program, Scope *global) {
    int errors = 0;

    for (int i = 0; i < program->nchildren; i++) {
        ASTNode *decl = program->children[i];
        const char *name = decl->ident;
        if (!name) continue;

        Symbol sym = {0};
        sym.dataType = decl->dataType;

        switch (decl->kind) {
            case ND_FUNC_DECL:
                errors += st_process_func_decl(decl, &sym, name);
                break;

            case ND_VAR_DECL:
                if (decl->isArray && decl->arraySize <= 0) {
                    ec_report("Errore: dimensione array non valida per '%s'\n", name);
                    errors++;
                    continue;
                }
                st_init_var_symbol(&sym, decl->isArray, decl->arraySize);
                break;

            default:
                continue;
        }

        if (st_bind_global_symbol(global, name, &sym) == 0) {
            decl->scopeLevel = sym.scopeLevel;
            decl->offset     = sym.offset;
        } else {
            errors++;
        }
    }

    return errors;
}
