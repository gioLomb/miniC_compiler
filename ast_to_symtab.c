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

/*
 * Il parser (vedi ParseDeclaration in parser.c) memorizza le dichiarazioni
 * come un'unica stringa nel campo 'text' del nodo:
 *   "int x"        -> variabile semplice
 *   "int arr[5]"   -> array
 *   "int somma"    -> nome di funzione (tipo di ritorno + nome)
 * Questa funzione la spacca nelle sue parti.
 */
static void parseDeclText(const char *text,
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

/*
 * Dichiara una singola variabile/parametro in 'scope', a partire dal
 * campo 'text' cosi' come lo produce il parser ("int x", "int arr[5]").
 * Riusata sia per ND_VAR_DECL (Pass 2, dentro un corpo) sia per ND_PARAM
 * (scope dei parametri): la logica di parsing/tipo e' identica, cambia
 * solo lo scope target e il messaggio d'errore stampato dal chiamante.
 */
static int declareVar(Scope *scope, const char *text) {
    char typeName[32], name[SYM_MAX_NAME_LEN];
    int isArray, arraySize;
    parseDeclText(text, typeName, sizeof(typeName), name, sizeof(name),
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
 * PASS 1 (interna): popola 'global' con le signature di TUTTE le
 * dichiarazioni top-level dell'AST (figli diretti di ND_PROGRAM), senza
 * scendere nei Block dei corpi funzione. Vedi ast_to_symtab.h per i
 * dettagli — qui resta invariata rispetto alla versione precedente,
 * solo non piu' pubblica: il chiamante esterno vede ormai solo
 * symtab_populate().
 */
static int populateGlobals(ASTNode *program, Scope *global) {
    int errors = 0;

    for (int i = 0; i < program->nchildren; i++) {
        ASTNode *decl = program->children[i];

        char typeName[32], name[SYM_MAX_NAME_LEN];
        int isArray, arraySize;
        parseDeclText(decl->text, typeName, sizeof(typeName), name, sizeof(name),
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
                parseDeclText(decl->children[p]->text, ptypeName, sizeof(ptypeName),
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

/*
 * PASS 2 (interna) — attraversamento ricorsivo del corpo di una funzione.
 *
 * Punto delicato: SOLO ND_BLOCK apre un nuovo scope. ND_IF e ND_WHILE
 * sono "trasparenti" rispetto allo scoping: ricorrono direttamente sui
 * rami (children[1] = then/body, children[2] = else se presente) passando
 * lo stesso 'scope' ricevuto, senza mai chiamare scope_create(). Se quel
 * ramo e' a sua volta un ND_BLOCK (cioe' nel sorgente c'erano le graffe),
 * sara' il case ND_BLOCK — non ND_IF/ND_WHILE — ad aprire lo scope figlio.
 * Questo riflette la semantica del C: "if (x) y = 1;" senza graffe non
 * introduce un livello di scoping, "if (x) { y = 1; }" si'.
 *
 * ND_RETURN/ND_EXPR_STMT non dichiarano nulla (le espressioni al loro
 * interno sono lookup, fuori dal perimetro di questa passata). Ogni altro
 * kind (ND_ERROR incluso) viene ignorato.
 */
static void walkStmt(ASTNode *stmt, Scope *scope, int *errors) {
    if (!stmt) return;

    switch (stmt->kind) {

    case ND_VAR_DECL:
        /* dichiara nello scope CORRENTE: non ne apre uno nuovo */
        if (!declareVar(scope, stmt->text)) (*errors)++;
        break;

    case ND_BLOCK: {
        /* l'UNICO caso che apre un nuovo scope */
        Scope *blockScope = scope_create(scope);
        for (int i = 0; i < stmt->nchildren; i++) {
            walkStmt(stmt->children[i], blockScope, errors);
        }
        break;
    }

    case ND_IF:
        /* niente scope_create qui: si ricorre con lo stesso scope */
        walkStmt(stmt->children[1], scope, errors);              /* then */
        if (stmt->nchildren > 2) {
            walkStmt(stmt->children[2], scope, errors);          /* else, se presente */
        }
        break;

    case ND_WHILE:
        walkStmt(stmt->children[1], scope, errors);               /* body */
        break;

    case ND_RETURN:
    case ND_EXPR_STMT:
        /* niente da dichiarare: le espressioni sono fuori perimetro (lookup) */
        break;

    default:
        /* ND_ERROR e altro: ignora */
        break;
    }
}

/*
 * PASS 2 (interna) — driver: per ogni ND_FUNC_DECL gia' registrato in
 * Pass 1, crea lo scope dei parametri (figlio di 'global') e attraversa
 * il corpo (l'ultimo figlio, sempre un ND_BLOCK) con walkStmt.
 */
static int populateFunctionBodies(ASTNode *program, Scope *global) {
    int errors = 0;

    for (int i = 0; i < program->nchildren; i++) {
        ASTNode *decl = program->children[i];
        if (decl->kind != ND_FUNC_DECL) continue;

        Scope *fnScope = scope_create(global);

        /* tutti i figli tranne l'ultimo sono ND_PARAM: l'ultimo e' il Block */
        int paramCount = decl->nchildren - 1;
        for (int p = 0; p < paramCount; p++) {
            /* un parametro duplicato e' una redeclaration nello stesso
               scope: symtab_declare la rifiuta gia' da solo, qui si
               controlla solo il valore di ritorno */
            if (!declareVar(fnScope, decl->children[p]->text)) errors++;
        }

        ASTNode *body = decl->children[decl->nchildren - 1];   /* ND_BLOCK */
        walkStmt(body, fnScope, &errors);
    }

    return errors;
}

/*
 * Interfaccia pubblica: vedi ast_to_symtab.h per la documentazione
 * completa. Le due passate restano funzioni separate solo internamente;
 * dal punto di vista del chiamante la popolazione della symtab a partire
 * dall'AST e' un'unica operazione.
 */
int symtab_populate(ASTNode *program, Scope *global) {
    int errors = 0;
    errors += populateGlobals(program, global);
    errors += populateFunctionBodies(program, global);
    return errors;
}