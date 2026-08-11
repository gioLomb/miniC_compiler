#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "semantic.h"
#include "ast_to_symtab.h"

/* Nome leggibile di un DataType, solo per messaggi d'errore. */
static inline const char *typeName(DataType t) {
    switch (t) {
        case T_INT:   return "int";
        case T_FLOAT: return "float";
        case T_VOID:  return "void";
        default:      return "?";
    }
}

/*
 * Regola di compatibilita' unica per assegnamenti, argomenti e return:
 * l'unica conversione implicita ammessa e' int -> float (widening).
 * Il contrario (float -> int, narrowing) e' sempre un errore, cosi'
 * come qualunque altra combinazione diversa dall'identita'.
 */
static inline int assignCompatible(DataType target, DataType value) {
    if (target == value) return 1;
    if (target == T_FLOAT && value == T_INT) return 1;
    return 0;
}

static DataType checkExpr(ASTNode *expr, Scope *scope, int *errors);

/*
 * ND_ID / ND_ARRAY_ACCESS condividono la stessa logica di lookup.
 */
static DataType checkNameUse(ASTNode *expr, Scope *scope, int wantArray, Symbol *outSym, int *errors) {
    Symbol sym;
    if (!symtab_lookup(scope, expr->text, &sym)) {
        fprintf(stderr, "Errore: '%s' non e' stato dichiarato\n", expr->text);
        (*errors)++;
        return T_VOID;
    }
    if (sym.kind != SYM_VAR) {
        fprintf(stderr, "Errore: '%s' e' una funzione, non una variabile\n", expr->text);
        (*errors)++;
        return T_VOID;
    }
    if (wantArray && !sym.isArray) {
        fprintf(stderr, "Errore: '%s' non e' un array, non si puo' usare con []\n", expr->text);
        (*errors)++;
        return T_VOID;
    }
    if (!wantArray && sym.isArray) {
        fprintf(stderr, "Errore: '%s' e' un array, va usato con [] e non da solo\n", expr->text);
        (*errors)++;
        return T_VOID;
    }
    expr->scopeLevel = sym.scopeLevel;
    expr->offset     = sym.offset;
    if (outSym) *outSym = sym;
    return sym.dataType;
}

/*
 * Se 'expr' e' una costante intera nota GIA' in fase di compilazione,
 * scrive il suo valore in *out e ritorna 1. Altrimenti ritorna 0.
 *
 * Usa confronto diretto su op[0] invece di strcmp: tutti gli operatori
 * sono caratteri singoli (o due char per >=, <=, ==, != — ma qui
 * interessano solo +, -, *, /, % che sono sempre un solo char).
 */
static int constIntValue(ASTNode *expr, long *out) {
    if (expr->kind == ND_NUM_INT) {
        *out = atol(expr->text);
        return 1;
    }
    if (expr->kind == ND_UNARY && expr->text[0] == '-' && expr->text[1] == '\0') {
        long inner;
        if (constIntValue(expr->children[0], &inner)) {
            *out = -inner;
            return 1;
        }
        return 0;
    }
    if (expr->kind == ND_BINOP) {
        long a, b;
        if (!constIntValue(expr->children[0], &a)) return 0;
        if (!constIntValue(expr->children[1], &b)) return 0;

        /* Tutti gli operatori aritmetici sono un solo char: confronto diretto */
        switch (expr->text[0]) {
        case '+': *out = a + b; return 1;
        case '-': *out = a - b; return 1;
        case '*': *out = a * b; return 1;
        case '/':
            if (b == 0) return 0;
            *out = a / b;
            return 1;
        case '%':
            if (b == 0) return 0;
            *out = a % b;
            return 1;
        default:
            /* confronto/logici: non rilevanti come indice, non foldati qui */
            return 0;
        }
    }
    return 0;
}

/*
 * Attraversa un'espressione, risolvendo ogni nome incontrato e
 * verificando la compatibilita' dei tipi.
 */
static DataType checkExpr(ASTNode *expr, Scope *scope, int *errors) {
    if (!expr) return T_VOID;

    switch (expr->kind) {

    case ND_NUM_INT:
        return T_INT;

    case ND_NUM_FLOAT:
        return T_FLOAT;

    case ND_ID:
        return checkNameUse(expr, scope, 0, NULL, errors);

    case ND_ARRAY_ACCESS: {
        ASTNode *idx = expr->children[0];
        DataType idxType = checkExpr(idx, scope, errors);
        if (idxType != T_VOID && idxType != T_INT) {
            fprintf(stderr, "Errore: l'indice di '%s[]' deve essere int, non %s\n",
                    expr->text, typeName(idxType));
            (*errors)++;
        }

        Symbol sym;
        DataType elemType = checkNameUse(expr, scope, 1, &sym, errors);

        long constValue;
        if (elemType != T_VOID && constIntValue(idx, &constValue)) {
            if (constValue < 0 || constValue >= sym.arraySize) {
                fprintf(stderr,
                        "Errore: indice %ld fuori dai limiti di '%s' (dimensione %d)\n",
                        constValue, expr->text, sym.arraySize);
                (*errors)++;
            }
        }

        return elemType;
    }

    case ND_CALL: {
        Symbol sym;
        int found = symtab_lookup(scope, expr->text, &sym);

        if (!found) {
            fprintf(stderr, "Errore: funzione '%s' non e' stata dichiarata\n", expr->text);
            (*errors)++;
        } else if (sym.kind != SYM_FUNC) {
            fprintf(stderr, "Errore: '%s' non e' una funzione\n", expr->text);
            (*errors)++;
            found = 0;
        } else if (expr->nchildren != sym.paramCount) {
            fprintf(stderr, "Errore: '%s' chiamata con %d argomenti, ne servono %d\n",
                    expr->text, expr->nchildren, sym.paramCount);
            (*errors)++;
        }

        for (int i = 0; i < expr->nchildren; i++) {
            DataType argType = checkExpr(expr->children[i], scope, errors);
            if (found && i < sym.paramCount) {
                DataType paramType = symtab_unpack_param_type(sym.paramTypes, i);
                if (argType != T_VOID && !assignCompatible(paramType, argType)) {
                    fprintf(stderr,
                            "Errore: argomento %d di '%s' e' %s, atteso %s\n",
                            i + 1, expr->text, typeName(argType), typeName(paramType));
                    (*errors)++;
                }
            }
        }

        return found ? sym.dataType : T_VOID;
    }

    case ND_ASSIGN: {
        ASTNode *lvalue = expr->children[0];
        ASTNode *rvalue = expr->children[1];

        if (lvalue->kind != ND_ID && lvalue->kind != ND_ARRAY_ACCESS) {
            fprintf(stderr, "Errore: il lato sinistro di '=' non e' una variabile valida\n");
            (*errors)++;
            checkExpr(rvalue, scope, errors);
            return T_VOID;
        }

        DataType lt = checkExpr(lvalue, scope, errors);
        DataType rt = checkExpr(rvalue, scope, errors);

        if (lt != T_VOID && rt != T_VOID && !assignCompatible(lt, rt)) {
            fprintf(stderr, "Errore: non si puo' assegnare %s a una variabile %s\n",
                    typeName(rt), typeName(lt));
            (*errors)++;
        }
        return lt;
    }

    case ND_BINOP: {
        DataType lt = checkExpr(expr->children[0], scope, errors);
        DataType rt = checkExpr(expr->children[1], scope, errors);

        /*
         * '%' e' sempre un solo char: confronto diretto su text[0].
         * Gli operatori binari del linguaggio con text[0]=='%' sono solo '%'.
         */
        if (expr->text[0] == '%' && expr->text[1] == '\0') {
            if (lt != T_VOID && lt != T_INT) { (*errors)++; }
            if (rt != T_VOID && rt != T_INT) { (*errors)++; }
            if ((lt != T_VOID && lt != T_INT) || (rt != T_VOID && rt != T_INT)) {
                fprintf(stderr, "Errore: l'operatore %% richiede operandi interi\n");
            }
            return T_INT;
        }

        /*
         * Confronto/logici: il risultato e' sempre un booleano (int).
         * Tutti questi operatori si distinguono per i primi due char:
         *   "==" text[0]='=' text[1]='='
         *   "!=" text[0]='!' text[1]='='
         *   "<"  text[0]='<' text[1]='\0'
         *   ">"  text[0]='>' text[1]='\0'
         *   "<=" text[0]='<' text[1]='='
         *   ">=" text[0]='>' text[1]='='
         *   "&&" text[0]='&' text[1]='&'
         *   "||" text[0]='|' text[1]='|'
         */
        switch (expr->text[0]) {
        case '=': /* "==" */
        case '!': /* "!=" */
        case '&': /* "&&" */
        case '|': /* "||" */
            return T_INT;
        case '<': /* "<" o "<=" */
        case '>': /* ">" o ">=" */
            return T_INT;
        default:
            break;
        }

        /* Aritmetici (+, -, *, /): int op int -> int, altrimenti -> float */
        if (lt == T_VOID || rt == T_VOID) return T_VOID;
        if (lt == T_FLOAT || rt == T_FLOAT) return T_FLOAT;
        return T_INT;
    }

    case ND_UNARY:
        /*
         * '!' e'-' sono sempre un solo char.
         * '!' -> risultato int (booleano)
         * '-' -> preserva tipo operando
         */
        if (expr->text[0] == '!' && expr->text[1] == '\0') {
            checkExpr(expr->children[0], scope, errors);
            return T_INT;
        }
        /* '-' unario: preserva il tipo dell'operando */
        return checkExpr(expr->children[0], scope, errors);

    default:
        fprintf(stderr, "Errore interno: nodo inatteso in un'espressione\n");
        (*errors)++;
        return T_VOID;
    }
}

/*
 * Attraversamento ricorsivo di uno statement.
 */
static void checkStmt(ASTNode *stmt, Scope *scope, DataType returnType, Arena *arena, int *errors) {
    if (!stmt) return;

    switch (stmt->kind) {

    case ND_VAR_DECL: {
        if (!symtab_declare_from_decl_text(arena, scope, stmt)) {
            (*errors)++;
            break;
        }
        if (stmt->nchildren == 0) break;

        char *typeNameBuf, *varName;
        int isArray, arraySize;
        symtab_parse_decl_text(arena, stmt->text, &typeNameBuf, &varName, &isArray, &arraySize);
        DataType declType = symtab_type_from_string(typeNameBuf);

        if (!isArray) {
            DataType t = checkExpr(stmt->children[0], scope, errors);
            if (t != T_VOID && !assignCompatible(declType, t)) {
                fprintf(stderr,
                        "Errore: non si puo' inizializzare '%s' (%s) con un valore %s\n",
                        varName, typeName(declType), typeName(t));
                (*errors)++;
            }
        } else {
            if (stmt->nchildren > arraySize) {
                fprintf(stderr,
                        "Errore: troppi inizializzatori per '%s' (%d forniti, dimensione %d)\n",
                        varName, stmt->nchildren, arraySize);
                (*errors)++;
            }
            for (int i = 0; i < stmt->nchildren; i++) {
                DataType t = checkExpr(stmt->children[i], scope, errors);
                if (t != T_VOID && !assignCompatible(declType, t)) {
                    fprintf(stderr,
                            "Errore: elemento %d dell'inizializzatore di '%s' e' %s, atteso %s\n",
                            i, varName, typeName(t), typeName(declType));
                    (*errors)++;
                }
            }
        }
        break;
    }

    case ND_BLOCK: {
        Scope *blockScope = scope_create(scope);
        for (int i = 0; i < stmt->nchildren; i++) {
            checkStmt(stmt->children[i], blockScope, returnType, arena, errors);
        }
        break;
    }

    case ND_IF:
        checkExpr(stmt->children[0], scope, errors);
        checkStmt(stmt->children[1], scope, returnType, arena, errors);
        if (stmt->nchildren > 2) {
            checkStmt(stmt->children[2], scope, returnType, arena, errors);
        }
        break;

    case ND_WHILE:
        checkExpr(stmt->children[0], scope, errors);
        checkStmt(stmt->children[1], scope, returnType, arena, errors);
        break;

    case ND_RETURN: {
        DataType t = checkExpr(stmt->children[0], scope, errors);
        if (t != T_VOID && !assignCompatible(returnType, t)) {
            fprintf(stderr, "Errore: return di tipo %s non compatibile col tipo %s della funzione\n",
                    typeName(t), typeName(returnType));
            (*errors)++;
        }
        break;
    }

    case ND_EXPR_STMT:
        checkExpr(stmt->children[0], scope, errors);
        break;

    default:
        break;
    }
}

/*
 * Driver: per ogni ND_FUNC_DECL crea lo scope dei parametri, vi dichiara
 * i parametri, ricava il tipo di ritorno e attraversa il corpo.
 */
static void checkFunctionBody(ASTNode *decl, Scope *global, Arena *arena, int *errors) {
    char *typeNameBuf, *funcName;
    int isArray, arraySize;
    symtab_parse_decl_text(arena, decl->text, &typeNameBuf, &funcName, &isArray, &arraySize);

    DataType returnType = symtab_type_from_string(typeNameBuf);

    Scope *fnScope = scope_create(global);

    int paramCount = decl->nchildren - 1;
    for (int p = 0; p < paramCount; p++) {
        if (!symtab_declare_from_decl_text(arena, fnScope, decl->children[p])) (*errors)++;
    }

    ASTNode *body = decl->children[decl->nchildren - 1];
    checkStmt(body, fnScope, returnType, arena, errors);
}

int semantic_check(ASTNode *program, Scope *global) {
    int errors = 0;
    Arena *arena = arena_create(0);

    for (int i = 0; i < program->nchildren; i++) {
        ASTNode *decl = program->children[i];
        if (decl->kind != ND_FUNC_DECL) continue;
        checkFunctionBody(decl, global, arena, &errors);
    }

    arena_destroy(arena);
    return errors;
}