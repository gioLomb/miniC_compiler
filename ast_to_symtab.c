#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast_to_symtab.h"

DataType symtab_type_from_string(const char *typeName) {
    if (!typeName) return T_VOID;
    if (strcmp(typeName, "int") == 0)   return T_INT;
    if (strcmp(typeName, "float") == 0) return T_FLOAT;
    return T_VOID;   /* fallback: copre anche "void" se/quando comparira' */
}

void symtab_parse_decl_text(Arena *arena, const char *text,
                             char **outTypeName, char **outName,
                             int *isArray, int *arraySize) {
    *isArray = 0;
    *arraySize = 0;
    *outTypeName = NULL;
    *outName = NULL;
    if (!text) return;

    /* Copia di lavoro nell'arena, esattamente della lunghezza di 'text':
       a differenza del vecchio "char buf[160]" non c'e' piu' nessun
       limite arbitrario da rispettare, qualunque sia la lunghezza reale
       della dichiarazione. La spacchiamo in-place scrivendo '\0' nei
       punti giusti (spazio, parentesi quadra), esattamente come prima -
       cambia solo da dove viene la memoria. */
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
        *arraySize = atoi(bracket + 1);   /* "5]" -> atoi legge "5" */
    } else {
        *outName = rest;
    }
}

int symtab_declare_from_decl_text(Arena *arena, Scope *scope, ASTNode *node) {
    char *typeName, *name;
    int isArray, arraySize;
    symtab_parse_decl_text(arena, node->text, &typeName, &name, &isArray, &arraySize);

    Symbol sym = {0};
    sym.kind = SYM_VAR;
    sym.dataType = symtab_type_from_string(typeName);
    sym.isArray = isArray;
    sym.arraySize = arraySize;
    sym.scopeLevel = scope->level;
    sym.offset     = (int) scope->table->size;   /* prossimo slot libero in questo scope */

    if (!symtab_declare(scope, name, &sym)) {
        fprintf(stderr, "Errore: '%s' e' gia' stato dichiarato in questo scope\n", name);
        return 0;
    }

    node->scopeLevel = sym.scopeLevel;
    node->offset     = sym.offset;
    return 1;
}

/*
 * PASS 1: vedi ast_to_symtab.h per la documentazione completa. Non
 * scende nei corpi funzione: quella parte (ex Pass 2) e' stata spostata
 * in semantic.c, fusa con l'analisi semantica.
 */
int symtab_populate_globals(ASTNode *program, Scope *global) {
    int errors = 0;
    Arena *arena = arena_create(0);

    for (int i = 0; i < program->nchildren; i++) {
        ASTNode *decl = program->children[i];

        char *typeName, *name;
        int isArray, arraySize;
        symtab_parse_decl_text(arena, decl->text, &typeName, &name, &isArray, &arraySize);

        Symbol sym = {0};
        sym.dataType = symtab_type_from_string(typeName);

        if (decl->kind == ND_FUNC_DECL) {
            sym.kind = SYM_FUNC;

            /* tutti i figli tranne l'ultimo sono ND_PARAM: l'ultimo e' il Block */
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
                symtab_parse_decl_text(arena, decl->children[p]->text,
                                        &ptypeName, &pname, &pIsArray, &pArraySize);

                DataType pType = symtab_type_from_string(ptypeName);
                symtab_pack_param_type(&sym.paramTypes, p, pType);
            }

        } else if (decl->kind == ND_VAR_DECL) {
            sym.kind = SYM_VAR;
            sym.isArray = isArray;
            sym.arraySize = arraySize;

        } else {
            /* nodo inatteso a livello globale (es. <error> da un recovery
               del parser): non c'e' nulla di sensato da registrare */
            continue;
        }

        if (!symtab_declare(global, name, &sym)) {
            fprintf(stderr, "Errore: '%s' e' gia' stato dichiarato in questo scope\n", name);
            errors++;
        }
    }

    arena_destroy(arena);
    return errors;
}
