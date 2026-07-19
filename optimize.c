#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "optimize.h"

/* ---- Helper di riconoscimento/lettura dei letterali ---------------- */

static int isIntLiteral(ASTNode *n)     { return n && n->kind == ND_NUM_INT; }
static int isFloatLiteral(ASTNode *n)   { return n && n->kind == ND_NUM_FLOAT; }
static int isNumericLiteral(ASTNode *n) { return isIntLiteral(n) || isFloatLiteral(n); }

static long   literalAsLong(ASTNode *n)   { return atol(n->text); }
static double literalAsDouble(ASTNode *n) { return atof(n->text); }
static int    literalIsZero(ASTNode *n)   { return literalAsDouble(n) == 0.0; }

/* Vero se 'n' e' un letterale INTERO che vale esattamente 'v'. Le
   semplificazioni algebriche (punto 2) sono limitate agli interi, quindi
   qui NON basta "valore numerico uguale" - deve essere proprio ND_NUM_INT. */
static int literalIntEquals(ASTNode *n, long v) {
    return isIntLiteral(n) && literalAsLong(n) == v;
}

/*
 * Libera SOLO questo nodo (il suo 'text' e il suo array 'children'), MAI
 * i figli puntati - da usare quando uno o piu' figli vengono riusati/
 * restituiti al chiamante e quindi non vanno distrutti insieme al nodo
 * che li conteneva. Contrappone a freeAST(), che invece libera l'intero
 * sottoalbero: quella si usa solo quando si e' sicuri che NESSUNO dei
 * figli sopravvive altrove.
 */
static void freeNodeShallow(ASTNode *node) {
    if (!node) return;
    free(node->children);
    free(node->text);
    free(node);
}

/*
 * Vero se valutare 'expr' potrebbe avere un effetto osservabile che NON
 * va eliminato silenziosamente semplificando un'espressione che lo
 * contiene (es. "f() * 0" non deve diventare "0": la chiamata a f() va
 * comunque eseguita). Deliberatamente conservativa: include non solo le
 * chiamate/assegnamenti ovvi, ma anche:
 *   - ND_ARRAY_ACCESS: un accesso il cui indice non e' costante potrebbe
 *     essere fuori dai bound a runtime (un trap, non solo un valore) -
 *     non possiamo provare il contrario qui, quindi lo trattiamo come
 *     "potenziale effetto collaterale".
 *   - ND_BINOP "/" o "%": una divisione il cui divisore non e' una
 *     costante nota potrebbe essere zero a runtime (altro possibile
 *     trap). Eliderla cambierebbe il comportamento osservabile del
 *     programma (un crash che sparisce silenziosamente).
 */
static int hasSideEffect(ASTNode *expr) {
    if (!expr) return 0;

    switch (expr->kind) {
        case ND_CALL:
        case ND_ASSIGN:
        case ND_ARRAY_ACCESS:
            return 1;
        case ND_BINOP:
            if (strcmp(expr->text, "/") == 0 || strcmp(expr->text, "%") == 0) {
                return 1;
            }
            break;
        default:
            break;
    }

    for (int i = 0; i < expr->nchildren; i++) {
        if (hasSideEffect(expr->children[i])) return 1;
    }
    return 0;
}

/* ---- Constant folding: costruzione del letterale risultato --------- */

/*
 * NOTA sui buffer a dimensione fissa qui sotto (char buf[32]/[64]): a
 * differenza dei buffer per stringhe arbitrarie (identificatori, ecc.)
 * che abbiamo sostituito con l'arena altrove nel progetto, qui si sta
 * formattando un NUMERO di ampiezza fissa (long a 64 bit, double stampato
 * con %g) - la lunghezza massima possibile e' matematicamente limitata
 * (un long a 64 bit occupa al massimo ~20 cifre inclusa l'eventuale
 * virgola), non indovinata: non e' lo stesso tipo di magic number che
 * avevamo eliminato altrove, quindi un buffer fisso qui e' legittimo.
 */

static ASTNode *foldBinopLiterals(const char *op, ASTNode *sx, ASTNode *dx) {
    int bothInt = isIntLiteral(sx) && isIntLiteral(dx);

    /* confronto/logici: risultato sempre int 0/1 (stessa convenzione di semantic.c) */
    if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
        strcmp(op, "<")  == 0 || strcmp(op, ">")  == 0 ||
        strcmp(op, "<=") == 0 || strcmp(op, ">=") == 0 ||
        strcmp(op, "&&") == 0 || strcmp(op, "||") == 0) {

        double a = literalAsDouble(sx), b = literalAsDouble(dx);
        int result;
        if      (strcmp(op, "==") == 0) result = (a == b);
        else if (strcmp(op, "!=") == 0) result = (a != b);
        else if (strcmp(op, "<")  == 0) result = (a < b);
        else if (strcmp(op, ">")  == 0) result = (a > b);
        else if (strcmp(op, "<=") == 0) result = (a <= b);
        else if (strcmp(op, ">=") == 0) result = (a >= b);
        else if (strcmp(op, "&&") == 0) result = (a != 0.0 && b != 0.0);
        else                             result = (a != 0.0 || b != 0.0); /* "||" */

        char buf[8];
        snprintf(buf, sizeof(buf), "%d", result);
        return newNode(ND_NUM_INT, buf);
    }

    if (strcmp(op, "%") == 0) {
        long b = literalAsLong(dx);
        if (b == 0) return NULL;   /* non foldare: modulo per zero, lascialo al runtime */
        long a = literalAsLong(sx);
        char buf[32];
        snprintf(buf, sizeof(buf), "%ld", a % b);
        return newNode(ND_NUM_INT, buf);
    }

    if (strcmp(op, "/") == 0) {
        if (bothInt) {
            long b = literalAsLong(dx);
            if (b == 0) return NULL;
            long a = literalAsLong(sx);
            char buf[32];
            snprintf(buf, sizeof(buf), "%ld", a / b);
            return newNode(ND_NUM_INT, buf);
        }
        double b = literalAsDouble(dx);
        if (b == 0.0) return NULL;
        double a = literalAsDouble(sx);
        char buf[64];
        snprintf(buf, sizeof(buf), "%g", a / b);
        return newNode(ND_NUM_FLOAT, buf);
    }

    /* + - * : int op int -> int; se almeno un operando e' float -> float
       (stessa regola di promozione usata in semantic.c per ND_BINOP) */
    if (bothInt) {
        long a = literalAsLong(sx), b = literalAsLong(dx), r;
        if      (strcmp(op, "+") == 0) r = a + b;
        else if (strcmp(op, "-") == 0) r = a - b;
        else                            r = a * b;   /* "*" */
        char buf[32];
        snprintf(buf, sizeof(buf), "%ld", r);
        return newNode(ND_NUM_INT, buf);
    }
    double a = literalAsDouble(sx), b = literalAsDouble(dx), r;
    if      (strcmp(op, "+") == 0) r = a + b;
    else if (strcmp(op, "-") == 0) r = a - b;
    else                            r = a * b;   /* "*" */
    char buf[64];
    snprintf(buf, sizeof(buf), "%g", r);
    return newNode(ND_NUM_FLOAT, buf);
}

static ASTNode *foldUnaryLiteral(const char *op, ASTNode *child) {
    if (strcmp(op, "-") == 0) {
        if (isIntLiteral(child)) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%ld", -literalAsLong(child));
            return newNode(ND_NUM_INT, buf);
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "%g", -literalAsDouble(child));
        return newNode(ND_NUM_FLOAT, buf);
    }
    if (strcmp(op, "!") == 0) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", literalIsZero(child) ? 1 : 0);
        return newNode(ND_NUM_INT, buf);
    }
    return NULL;
}

/* ---- Tree height balancing per catene associative (+, *) ----------- */

/*
 * Vero se 'n' contiene, in QUALUNQUE punto del suo sottoalbero, un
 * letterale ND_NUM_FLOAT - non solo come foglia diretta di una catena
 * '+' o '*', ma annidato dentro una sotto-espressione con un operatore diverso
 * (es. "a + (x*1.5) + b": il letterale 1.5 non e' foglia diretta della
 * catena "+", ma la rende comunque "contaminata" di virgola mobile).
 *
 * LIMITE NOTO (vedi discussione): rileva solo i letterali float VISIBILI
 * nel testo. Una catena di sole variabili "float" (nessun letterale in
 * vista) non viene riconosciuta come tale - servirebbe conoscere il tipo
 * delle variabili, e optimize.c non ha accesso alla symbol table (nessuno
 * Scope* nella sua firma). Bilanciare una simile catena cambierebbe
 * l'arrotondamento del risultato senza che questa guardia se ne accorga.
 * Risolvibile solo estendendo optimize_ast con un vero Scope*, rimandato.
 */
static int containsFloatLiteral(ASTNode *n) {
    if (!n) return 0;
    if (n->kind == ND_NUM_FLOAT) return 1;
    for (int i = 0; i < n->nchildren; i++) {
        if (containsFloatLiteral(n->children[i])) return 1;
    }
    return 0;
}

/*
 * Appiattisce ricorsivamente la catena associativa che ha 'node' come
 * radice, raccogliendo in '*leaves' (array a raddoppio, come altrove nel
 * progetto) tutte le foglie in ordine SINISTRA-DESTRA - lo stesso ordine
 * di valutazione originale: bilanciare cambia solo la FORMA dell'albero,
 * mai l'ordine in cui le foglie vengono valutate (per questo non serve
 * controllare hasSideEffect() qui, a differenza delle altre
 * semplificazioni in questo file). Scende solo finche' incontra nodi
 * ND_BINOP con lo STESSO operatore testuale di 'op' - un operatore
 * diverso (anche se associativo a sua volta) e' un'altra catena, e resta
 * una foglia opaca per questa. I nodi ND_BINOP intermedi della catena
 * originale vengono liberati (freeNodeShallow: le foglie sopravvivono,
 * riusate cosi' come sono).
 */
static void flattenChain(ASTNode *node, const char *op, ASTNode ***leaves, int *count, int *cap) {
    if (node->kind == ND_BINOP && strcmp(node->text, op) == 0) {
        flattenChain(node->children[0], op, leaves, count, cap);
        flattenChain(node->children[1], op, leaves, count, cap);
        freeNodeShallow(node);
    } else {
        if (*count == *cap) {
            *cap = *cap ? *cap * 2 : 4;
            *leaves = realloc(*leaves, (size_t) *cap * sizeof(ASTNode *));
        }
        (*leaves)[(*count)++] = node;
    }
}

/*
 * Ricostruisce un albero bilanciato da un array piatto di foglie (gia'
 * in ordine sinistra-destra), appaiandole a due a due un livello alla
 * volta - come costruire un min-heap da un array. Con 'count' foglie
 * produce un albero di altezza ~log2(count) invece di 'count'-1.
 * Se 'count' e' dispari, l'ultima foglia del livello sale al livello
 * successivo senza appaiarsi (nessuna foglia viene mai duplicata o
 * persa). Assume costo/altezza uniforme delle foglie (limite noto: non
 * ottimale se una foglia e' una sotto-espressione molto piu' profonda
 * delle altre - correttezza non compromessa, solo bilanciamento subottimale
 * in quel caso).
 */
static ASTNode *buildBalanced(ASTNode **leaves, int count, const char *op) {
    if (count == 1) return leaves[0];

    int nextCount = (count + 1) / 2;
    ASTNode **next = malloc((size_t) nextCount * sizeof(ASTNode *));
    int idx = 0;
    int i = 0;
    for (; i + 1 < count; i += 2) {
        ASTNode *pair = newNode(ND_BINOP, op);
        addChild(pair, leaves[i]);
        addChild(pair, leaves[i + 1]);
        next[idx++] = pair;
    }
    if (i < count) {
        next[idx++] = leaves[i];   /* foglia dispari: sale invariata */
    }

    ASTNode *result = buildBalanced(next, nextCount, op);
    free(next);
    return result;
}

/*
 * Punto d'ingresso: se 'expr' e' la radice di una catena '+' o '*' su soli
 * interi (nessun letterale float visibile, vedi containsFloatLiteral),
 * la appiattisce e la ricostruisce bilanciata. Altrimenti restituisce
 * 'expr' invariato. Va chiamata a valle del folding/delle semplificazioni
 * algebriche gia' esistenti (solo se 'expr' sopravvive come vero ND_BINOP
 * ha senso provare a bilanciarlo).
 */
static ASTNode *balanceAssocChain(ASTNode *expr) {
    if (strcmp(expr->text, "+") != 0 && strcmp(expr->text, "*") != 0) return expr;
    if (containsFloatLiteral(expr)) return expr;

    /* Copia l'operatore PRIMA di iniziare a smontare la catena: expr
       stesso viene liberato (freeNodeShallow, incluso expr->text) dentro
       flattenChain non appena verifica che la radice fa parte della
       catena - rileggere expr->text dopo quella chiamata sarebbe un
       use-after-free (trovato con AddressSanitizer). '+' e '*' sono
       sempre un solo carattere (verificato dal controllo sopra), quindi
       un buffer di 2 byte basta sempre. */
    char op[2] = { expr->text[0], '\0' };

    ASTNode **leaves = NULL;
    int count = 0, cap = 0;
    flattenChain(expr, op, &leaves, &count, &cap);

    ASTNode *result = buildBalanced(leaves, count, op);
    free(leaves);
    return result;
}

/* ---- Attraversamento delle espressioni ------------------------------ */

static ASTNode *optimizeExpr(ASTNode *expr) {
    if (!expr) return NULL;

    switch (expr->kind) {

    case ND_NUM_INT:
    case ND_NUM_FLOAT:
    case ND_ID:
        return expr;   /* foglie: niente da fare */

    case ND_UNARY: {
        expr->children[0] = optimizeExpr(expr->children[0]);
        ASTNode *child = expr->children[0];

        if (isNumericLiteral(child)) {
            ASTNode *folded = foldUnaryLiteral(expr->text, child);
            if (folded) {
                freeAST(expr);   /* libera sia lo UNARY sia il child: nessuno dei due sopravvive */
                return folded;
            }
        }
        return expr;
    }

    case ND_BINOP: {
        expr->children[0] = optimizeExpr(expr->children[0]);
        expr->children[1] = optimizeExpr(expr->children[1]);
        ASTNode *sx = expr->children[0];
        ASTNode *dx = expr->children[1];

        /* 1) folding completo: entrambi gli operandi sono gia' letterali */
        if (isNumericLiteral(sx) && isNumericLiteral(dx)) {
            ASTNode *folded = foldBinopLiterals(expr->text, sx, dx);
            if (folded) {
                freeAST(expr);   /* libera BINOP + entrambi gli operandi */
                return folded;
            }
            /* folded == NULL: divisione/modulo per zero -> non tocco
               nulla, verra' valutata (e fallira' correttamente) a runtime */
        }

        /* 2) semplificazioni algebriche, solo T_INT, un solo lato costante */
        if (strcmp(expr->text, "+") == 0) {
            if (literalIntEquals(dx, 0)) { freeNodeShallow(dx); freeNodeShallow(expr); return sx; }
            if (literalIntEquals(sx, 0)) { freeNodeShallow(sx); freeNodeShallow(expr); return dx; }

        } else if (strcmp(expr->text, "-") == 0) {
            if (literalIntEquals(dx, 0)) { freeNodeShallow(dx); freeNodeShallow(expr); return sx; }

        } else if (strcmp(expr->text, "*") == 0) {
            if (literalIntEquals(dx, 1)) { freeNodeShallow(dx); freeNodeShallow(expr); return sx; }
            if (literalIntEquals(sx, 1)) { freeNodeShallow(sx); freeNodeShallow(expr); return dx; }

            /* x*0 / 0*x: elide la valutazione dell'altro operando - lecito
               SOLO se quell'operando e' provatamente senza effetti
               collaterali (vedi hasSideEffect sopra) */
            if (literalIntEquals(dx, 0) && !hasSideEffect(sx)) {
                freeAST(expr);   /* qui si butta via anche sx: e' provato innocuo */
                return newNode(ND_NUM_INT, "0");
            }
            if (literalIntEquals(sx, 0) && !hasSideEffect(dx)) {
                freeAST(expr);
                return newNode(ND_NUM_INT, "0");
            }
        }

        /* 3) tree height balancing: solo se 'expr' e' sopravvissuto come
           vero ND_BINOP fin qui (nessun folding/semplificazione l'ha gia'
           eliminato) */
        return balanceAssocChain(expr);
    }

    case ND_ASSIGN:
        expr->children[0] = optimizeExpr(expr->children[0]);   /* lvalue (es. indice di un array) */
        expr->children[1] = optimizeExpr(expr->children[1]);   /* rvalue */
        return expr;

    case ND_CALL:
        for (int i = 0; i < expr->nchildren; i++) {
            expr->children[i] = optimizeExpr(expr->children[i]);
        }
        return expr;

    case ND_ARRAY_ACCESS:
        expr->children[0] = optimizeExpr(expr->children[0]);   /* indice */
        return expr;

    default:
        return expr;
    }
}

/* ---- Attraversamento degli statement --------------------------------
 *
 * optimizeStmt ritorna il nodo che deve sostituire 'stmt' nel genitore,
 * oppure NULL per dire "rimuovi questo statement, non lo sostituire con
 * nulla" (es. un intero "while(0) { ... }" che sparisce). Chi chiama
 * (il caso ND_BLOCK, o optimize_ast per il corpo di una funzione) deve
 * gestire esplicitamente il caso NULL.
 */
static ASTNode *optimizeStmt(ASTNode *stmt) {
    if (!stmt) return NULL;

    switch (stmt->kind) {

    case ND_VAR_DECL:
        for (int i = 0; i < stmt->nchildren; i++) {
            stmt->children[i] = optimizeExpr(stmt->children[i]);
        }
        return stmt;

    case ND_BLOCK: {
        /* Ricostruiamo l'array children da zero (con addChild, che gestisce
           gia' da sola la crescita dell'array): se il risultato per un
           certo statement e' esso stesso un ND_BLOCK (tipicamente il
           superstite di un if/else appena collassato, es. "else { a=2; }"
           che sopravvive cosi' com'era scritto nel sorgente), i suoi
           figli vengono "spianati" direttamente qui invece di restare
           annidati in un wrapper superfluo - un ND_BLOCK dentro un
           ND_BLOCK e' innocuo ma inutile: lo eliminiamo per avere un
           albero piu' pulito da passare alla generazione dell'IR. */
        ASTNode **oldChildren = stmt->children;
        int oldCount = stmt->nchildren;

        stmt->children = NULL;
        stmt->nchildren = 0;
        stmt->capacity = 0;

        for (int i = 0; i < oldCount; i++) {
            ASTNode *result = optimizeStmt(oldChildren[i]);
            if (!result) continue;

            if (result->kind == ND_BLOCK) {
                /* i nipoti diventano figli diretti; il wrapper si libera
                   (solo se stesso: i suoi figli sopravvivono, riusati qui) */
                for (int j = 0; j < result->nchildren; j++) {
                    addChild(stmt, result->children[j]);
                }
                freeNodeShallow(result);
            } else {
                addChild(stmt, result);
            }
        }

        free(oldChildren);
        return stmt;
    }

    case ND_IF: {
        stmt->children[0] = optimizeExpr(stmt->children[0]);
        ASTNode *cond = stmt->children[0];

        if (isNumericLiteral(cond)) {
            /* condizione nota a compile-time: uno dei due rami e'
               provatamente morto, l'intero ND_IF si riduce al superstite */
            int condTrue = !literalIsZero(cond);
            ASTNode *thenBr = stmt->children[1];
            ASTNode *elseBr = (stmt->nchildren > 2) ? stmt->children[2] : NULL;

            ASTNode *survivor = condTrue ? thenBr : elseBr;
            ASTNode *deadBranch = condTrue ? elseBr : thenBr;

            freeAST(cond);
            if (deadBranch) freeAST(deadBranch);
            freeNodeShallow(stmt);   /* mai freeAST(stmt): 'survivor' deve restare vivo */

            return survivor ? optimizeStmt(survivor) : NULL;
        }

        /* condizione non costante: resta un vero if a runtime, ma i due
           rami vengono comunque ottimizzati ricorsivamente */
        stmt->children[1] = optimizeStmt(stmt->children[1]);
        if (!stmt->children[1]) stmt->children[1] = newNode(ND_BLOCK, NULL);

        if (stmt->nchildren > 2) {
            stmt->children[2] = optimizeStmt(stmt->children[2]);
            if (!stmt->children[2]) stmt->children[2] = newNode(ND_BLOCK, NULL);
        }
        return stmt;
    }

    case ND_WHILE: {
        stmt->children[0] = optimizeExpr(stmt->children[0]);
        ASTNode *cond = stmt->children[0];

        if (isNumericLiteral(cond) && literalIsZero(cond)) {
            /* while(0): il corpo non viene mai eseguito, l'intero ciclo e' morto */
            freeAST(stmt);
            return NULL;
        }

        stmt->children[1] = optimizeStmt(stmt->children[1]);
        if (!stmt->children[1]) stmt->children[1] = newNode(ND_BLOCK, NULL);
        return stmt;
    }

    case ND_RETURN:
    case ND_EXPR_STMT:
        stmt->children[0] = optimizeExpr(stmt->children[0]);
        return stmt;

    default:
        return stmt;   /* ND_ERROR e altro: lascia invariato */
    }
}

void optimize_ast(ASTNode *program) {
    if (!program) return;

    for (int i = 0; i < program->nchildren; i++) {
        ASTNode *decl = program->children[i];
        if (!decl) continue;

        if (decl->kind == ND_FUNC_DECL) {
            /* il corpo e' sempre l'ultimo figlio (vedi ast_to_symtab.c/
               semantic.c: stessa convenzione riusata qui) */
            ASTNode *body = decl->children[decl->nchildren - 1];
            ASTNode *optimizedBody = optimizeStmt(body);
            if (!optimizedBody) optimizedBody = newNode(ND_BLOCK, NULL);
            decl->children[decl->nchildren - 1] = optimizedBody;

        } else if (decl->kind == ND_VAR_DECL) {
            /* variabile globale con inizializzatore: ottimizza anche quello */
            for (int c = 0; c < decl->nchildren; c++) {
                decl->children[c] = optimizeExpr(decl->children[c]);
            }
        }
    }
}