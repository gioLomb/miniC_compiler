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

void symtab_parse_decl_text(const char *text,
                             char *typeName, size_t typeCap,
                             char *name, size_t nameCap,
                             int *isArray, int *arraySize) {
    *isArray = 0;
    *arraySize = 0;
    typeName[0] = '\0';
    name[0] = '\0';
    if (!text) return;

    char buf[160];
    snprintf(buf, sizeof(buf), "%s", text);

    char *space = strchr(buf, ' ');
    if (!space) {
        snprintf(typeName, typeCap, "%s", buf);
        return;
    }
    *space = '\0';
    snprintf(typeName, typeCap, "%s", buf);

    char *rest = space + 1;
    char *bracket = strchr(rest, '[');
    if (bracket) {
        *bracket = '\0';
        snprintf(name, nameCap, "%s", rest);
        *isArray = 1;
        *arraySize = atoi(bracket + 1);   /* "5]" -> atoi legge "5" */
    } else {
        snprintf(name, nameCap, "%s", rest);
    }
}

int symtab_declare_from_decl_text(Scope *scope, const char *text) {
    char typeName[32], name[SYM_MAX_NAME_LEN];
    int isArray, arraySize;
    symtab_parse_decl_text(text, typeName, sizeof(typeName), name, sizeof(name),
                            &isArray, &arraySize);

    Symbol sym = {0};
    sym.kind = SYM_VAR;
    sym.dataType = symtab_type_from_string(typeName);
    sym.isArray = isArray;
    sym.arraySize = arraySize;

    if (!symtab_declare(scope, name, &sym)) {
        fprintf(stderr, "Errore: '%s' e' gia' stato dichiarato in questo scope\n", name);
        return 0;
    }
    return 1;
}

/*
 * PASS 1: vedi ast_to_symtab.h per la documentazione completa. Non
 * scende nei corpi funzione: quella parte (ex Pass 2) e' stata spostata
 * in semantic.c, fusa con l'analisi semantica.
 */
int symtab_populate_globals(ASTNode *program, Scope *global) {
    int errors = 0;

    for (int i = 0; i < program->nchildren; i++) {
        ASTNode *decl = program->children[i];

        char typeName[32], name[SYM_MAX_NAME_LEN];
        int isArray, arraySize;
        symtab_parse_decl_text(decl->text, typeName, sizeof(typeName), name, sizeof(name),
                                &isArray, &arraySize);

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
                char ptypeName[32], pname[SYM_MAX_NAME_LEN];
                int pIsArray, pArraySize;
                symtab_parse_decl_text(decl->children[p]->text, ptypeName, sizeof(ptypeName),
                                        pname, sizeof(pname), &pIsArray, &pArraySize);

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

    return errors;
}
