#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "semantic.h"
#include "ast_to_symtab.h"

/* Nome leggibile di un DataType, solo per messaggi d'errore. */
static const char *typeName(DataType t) {
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
static int assignCompatible(DataType target, DataType value) {
    if (target == value) return 1;
    if (target == T_FLOAT && value == T_INT) return 1;
    return 0;
}

static DataType checkExpr(ASTNode *expr, Scope *scope, int *errors);

/*
 * ND_ID / ND_ARRAY_ACCESS condividono la stessa logica di lookup: qui
 * factorizzata per non duplicarla. 'wantArray' indica quale dei due casi
 * stiamo verificando (1 = ND_ARRAY_ACCESS, 0 = ND_ID). 'outSym', se non
 * NULL, riceve il Symbol trovato quando il lookup ha successo — serve
 * al chiamante ND_ARRAY_ACCESS per leggere arraySize (bound-check
 * statico) senza rifare un secondo symtab_lookup identico.
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
    if (outSym) *outSym = sym;
    return sym.dataType;
}

/*
 * Se 'expr' e' una costante intera nota GIA' in fase di compilazione
 * (un letterale, eventualmente con uno o piu' meno unari davanti, es.
 * "3", "-1", "- -2"), scrive il suo valore in *out e ritorna 1.
 * Altrimenti (una variabile, una chiamata, un'espressione con operandi
 * non costanti...) ritorna 0: quell'indice si potra' verificare solo a
 * runtime — fuori portata di un'analisi statica, e non e' un errore in
 * se': significa solo "qui non possiamo dire nulla in anticipo".
 */
static int constIntValue(ASTNode *expr, long *out) {
    if (expr->kind == ND_NUM_INT) {
        *out = atol(expr->text);
        return 1;
    }
    if (expr->kind == ND_UNARY && strcmp(expr->text, "-") == 0) {
        long inner;
        if (constIntValue(expr->children[0], &inner)) {
            *out = -inner;
            return 1;
        }
    }
    return 0;
}

/*
 * Attraversa un'espressione, risolvendo ogni nome incontrato e
 * verificando la compatibilita' dei tipi. Restituisce il DataType
 * risultante dell'espressione (T_VOID in caso di errore, cosi' che gli
 * errori non si propaghino a cascata in falsi positivi sui livelli
 * superiori: un T_VOID non fa mai scattare un ulteriore errore di tipo,
 * dato che assignCompatible/i confronti espliciti lo trattano come
 * "sconosciuto" e le chiamate successive lo accettano implicitamente
 * solo per non moltiplicare i messaggi per un singolo errore reale).
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

        /* Bound-check statico: solo se l'indice e' una costante intera
           nota adesso (letterale, eventualmente con un meno unario).
           Un indice calcolato a runtime (variabile, espressione) non
           puo' essere verificato qui - richiederebbe un controllo nel
           codice generato, non un'analisi statica. */
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

        /* verifica comunque ogni argomento (per riportare piu' errori in
           un colpo solo), confrontando col parametro corrispondente solo
           se il lookup e' andato a buon fine e l'indice e' in range */
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

        if (strcmp(expr->text, "%") == 0) {
            if (lt != T_VOID && lt != T_INT) { (*errors)++; }
            if (rt != T_VOID && rt != T_INT) { (*errors)++; }
            if ((lt != T_VOID && lt != T_INT) || (rt != T_VOID && rt != T_INT)) {
                fprintf(stderr, "Errore: l'operatore %% richiede operandi interi\n");
            }
            return T_INT;
        }

        /* confronto/logici: il risultato e' sempre un booleano (int) */
        if (strcmp(expr->text, "==") == 0 || strcmp(expr->text, "!=") == 0 ||
            strcmp(expr->text, "<")  == 0 || strcmp(expr->text, ">")  == 0 ||
            strcmp(expr->text, "<=") == 0 || strcmp(expr->text, ">=") == 0 ||
            strcmp(expr->text, "&&") == 0 || strcmp(expr->text, "||") == 0) {
            return T_INT;
        }

        /* aritmetici (+ - * /): int op int -> int, altrimenti (se almeno
           un operando e' float) -> float. T_VOID (gia' segnalato piu'
           in basso) non genera un ulteriore errore qui. */
        if (lt == T_VOID || rt == T_VOID) return T_VOID;
        if (lt == T_FLOAT || rt == T_FLOAT) return T_FLOAT;
        return T_INT;
    }

    case ND_UNARY:
        if (strcmp(expr->text, "!") == 0) {
            checkExpr(expr->children[0], scope, errors);
            return T_INT;
        }
        /* '-' unario: preserva il tipo dell'operando */
        return checkExpr(expr->children[0], scope, errors);

    default:
        /* non dovrebbe capitare in un'espressione ben formata dal parser */
        fprintf(stderr, "Errore interno: nodo inatteso in un'espressione\n");
        (*errors)++;
        return T_VOID;
    }
}

/*
 * Attraversamento ricorsivo di uno statement: fonde quello che prima
 * era walkStmt (Pass 2: dichiarazione di variabili, apertura scope sui
 * ND_BLOCK) con la verifica delle espressioni al suo interno.
 *
 * Punto delicato invariato rispetto alla vecchia Pass 2: SOLO ND_BLOCK
 * apre un nuovo scope. ND_IF/ND_WHILE ricorrono sui rami passando lo
 * stesso 'scope' ricevuto, senza mai chiamare scope_create(): uno
 * statement senza graffe non introduce un livello di scoping.
 *
 * 'returnType' e' il tipo di ritorno della funzione in cui ci troviamo
 * (serve a ND_RETURN); si propaga invariato attraverso ricorsione,
 * esattamente come lo scope.
 */
static void checkStmt(ASTNode *stmt, Scope *scope, DataType returnType, Arena *arena, int *errors) {
    if (!stmt) return;

    switch (stmt->kind) {

    case ND_VAR_DECL: {
        /* dichiara nello scope CORRENTE: non ne apre uno nuovo */
        if (!symtab_declare_from_decl_text(arena, scope, stmt->text)) {
            (*errors)++;
            break;
        }
        if (stmt->nchildren == 0) break;   /* nessun inizializzatore */

        /* Ricava tipo/isArray/arraySize dallo stesso testo appena usato
           per dichiarare il simbolo: piu' semplice che rifare un lookup. */
        char *typeNameBuf, *varName;
        int isArray, arraySize;
        symtab_parse_decl_text(arena, stmt->text, &typeNameBuf, &varName, &isArray, &arraySize);
        DataType declType = symtab_type_from_string(typeNameBuf);

        if (!isArray) {
            /* scalare: esattamente un figlio (garantito dal parser) */
            DataType t = checkExpr(stmt->children[0], scope, errors);
            if (t != T_VOID && !assignCompatible(declType, t)) {
                fprintf(stderr,
                        "Errore: non si puo' inizializzare '%s' (%s) con un valore %s\n",
                        varName, typeName(declType), typeName(t));
                (*errors)++;
            }
        } else {
            /* array: un figlio per elemento, in ordine; troppi
               inizializzatori rispetto alla dimensione dichiarata e' un
               bound-check statico esattamente come per ND_ARRAY_ACCESS */
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
        /* l'UNICO caso che apre un nuovo scope */
        Scope *blockScope = scope_create(scope);
        for (int i = 0; i < stmt->nchildren; i++) {
            checkStmt(stmt->children[i], blockScope, returnType, arena, errors);
        }
        break;
    }

    case ND_IF:
        checkExpr(stmt->children[0], scope, errors);                        /* condizione */
        checkStmt(stmt->children[1], scope, returnType, arena, errors);      /* then */
        if (stmt->nchildren > 2) {
            checkStmt(stmt->children[2], scope, returnType, arena, errors); /* else, se presente */
        }
        break;

    case ND_WHILE:
        checkExpr(stmt->children[0], scope, errors);                        /* condizione */
        checkStmt(stmt->children[1], scope, returnType, arena, errors);      /* body */
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
        /* ND_ERROR e altro: ignora */
        break;
    }
}

/*
 * Ex Pass 2, driver: per ogni ND_FUNC_DECL crea lo scope dei parametri
 * (figlio di 'global'), vi dichiara i parametri, ricava il tipo di
 * ritorno dalla propria signature (gia' nota da Pass 1, ma qui la
 * rileggiamo dal testo del nodo perche' e' piu' semplice che tornare a
 * cercarla nello scope globale) e attraversa il corpo con checkStmt.
 */
static void checkFunctionBody(ASTNode *decl, Scope *global, Arena *arena, int *errors) {
    char *typeNameBuf, *funcName;
    int isArray, arraySize;
    symtab_parse_decl_text(arena, decl->text, &typeNameBuf, &funcName, &isArray, &arraySize);

    DataType returnType = symtab_type_from_string(typeNameBuf);

    Scope *fnScope = scope_create(global);

    /* tutti i figli tranne l'ultimo sono ND_PARAM: l'ultimo e' il Block */
    int paramCount = decl->nchildren - 1;
    for (int p = 0; p < paramCount; p++) {
        /* un parametro duplicato e' una redeclaration nello stesso
           scope: symtab_declare la rifiuta gia' da sola, qui si
           controlla solo il valore di ritorno */
        if (!symtab_declare_from_decl_text(arena, fnScope, decl->children[p]->text)) (*errors)++;
    }

    ASTNode *body = decl->children[decl->nchildren - 1];   /* ND_BLOCK */
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