#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lexer.h"
#include "parser/error.h"
#include "parser/ast.h"
#include "parser/parser.h"
#include "symbol_table.h"
#include "ast_to_symtab.h"
#include "semantic.h"
#include "optimize.h"

/* Scrive 'src' su un file temporaneo e fa girare l'intera pipeline fino
   a optimize_ast incluso; restituisce la radice dell'AST gia' ottimizzato
   (il chiamante e' responsabile di freeAST() a fine test). */
static ASTNode *parseAndOptimize(const char *src) {
    const char *path = "/tmp/miniC_test_optimize_src.c";
    FILE *f = fopen(path, "w");
    if (!f) { perror("fopen"); exit(1); }
    fputs(src, f);
    fclose(f);

    lexer_open(path);
    ASTNode *root = ParseProgram();
    lexer_close();

    Scope *global = scope_create(NULL);
    symtab_populate_globals(root, global);
    semantic_check(root, global);
    symtab_destroy_tree(global);

    optimize_ast(root);
    return root;
}

/* L'ultima dichiarazione top-level dei sorgenti di test qui sotto e'
   sempre la funzione che ci interessa ispezionare (in genere "main"). */
static ASTNode *lastFuncBody(ASTNode *root) {
    ASTNode *decl = root->children[root->nchildren - 1];
    return decl->children[decl->nchildren - 1];
}

static ASTNode *findFirstOfKind(ASTNode *node, NodeKind kind) {
    if (!node) return NULL;
    if (node->kind == kind) return node;
    for (int i = 0; i < node->nchildren; i++) {
        ASTNode *found = findFirstOfKind(node->children[i], kind);
        if (found) return found;
    }
    return NULL;
}

static int countOfKind(ASTNode *node, NodeKind kind) {
    if (!node) return 0;
    int count = (node->kind == kind) ? 1 : 0;
    for (int i = 0; i < node->nchildren; i++) {
        count += countOfKind(node->children[i], kind);
    }
    return count;
}

/* Raccoglie, in ordine di apparizione (DFS pre-order), tutti i nodi di
   tipo 'kind' in 'out' (capacita' massima 'maxOut'), aggiornando *count. */
static void collectOfKind(ASTNode *node, NodeKind kind, ASTNode **out, int maxOut, int *count) {
    if (!node) return;
    if (node->kind == kind && *count < maxOut) {
        out[(*count)++] = node;
    }
    for (int i = 0; i < node->nchildren; i++) {
        collectOfKind(node->children[i], kind, out, maxOut, count);
    }
}

/* Altezza del piu' lungo cammino di soli ND_BINOP a partire da 'node'
   (0 se 'node' non e' un ND_BINOP, cioe' e' gia' una foglia). Serve a
   verificare che il bilanciamento riduca davvero la profondita' della
   catena, non solo che il risultato resti corretto. */
static int binopHeight(ASTNode *node) {
    if (!node || node->kind != ND_BINOP) return 0;
    int l = binopHeight(node->children[0]);
    int r = binopHeight(node->children[1]);
    return 1 + (l > r ? l : r);
}

int main(void) {
    ASTNode *root, *body, *node;

    /* PASS 1: constant folding di superficie: z = 3 + 2; -> z = 5; */
    root = parseAndOptimize(
        "int main() { int z; z = 3 + 2; return z; }");
    body = lastFuncBody(root);
    node = findFirstOfKind(body, ND_ASSIGN);
    if (!node || node->children[1]->kind != ND_NUM_INT ||
        strcmp(node->children[1]->text, "5") != 0) {
        fprintf(stderr, "PASS 1 FALLITO\n");
        return 1;
    }
    printf("PASS 1 ok: 'z = 3 + 2;' viene foldato in 'z = 5;'.\n");
    freeAST(root);

    /* PASS 2: semplificazione algebrica x = y + 0; -> x = y; (solo int) */
    root = parseAndOptimize(
        "int main() { int x; int y; y = 1; x = y + 0; return x; }");
    body = lastFuncBody(root);
    /* il secondo ND_ASSIGN e' quello di 'x' (il primo e' 'y = 1;') */
    ASTNode *assigns[8]; int nAssigns = 0;
    collectOfKind(body, ND_ASSIGN, assigns, 8, &nAssigns);
    if (nAssigns < 2 || assigns[1]->children[1]->kind != ND_ID ||
        strcmp(assigns[1]->children[1]->text, "y") != 0) {
        fprintf(stderr, "PASS 2 FALLITO\n");
        return 1;
    }
    printf("PASS 2 ok: 'x = y + 0;' viene semplificato in 'x = y;'.\n");
    freeAST(root);

    /* PASS 3: pruning strutturale — if(0){a=1;}else{a=2;} sopravvive solo il ramo else */
    root = parseAndOptimize(
        "int main() { int a; if (0) { a = 1; } else { a = 2; } return a; }");
    body = lastFuncBody(root);
    if (countOfKind(body, ND_IF) != 0) {
        fprintf(stderr, "PASS 3 FALLITO: l'ND_IF non e' stato rimosso\n");
        return 1;
    }
    node = findFirstOfKind(body, ND_ASSIGN);
    if (!node || node->children[1]->kind != ND_NUM_INT ||
        strcmp(node->children[1]->text, "2") != 0) {
        fprintf(stderr, "PASS 3 FALLITO: non e' rimasto 'a = 2;'\n");
        return 1;
    }
    printf("PASS 3 ok: 'if(0){a=1;}else{a=2;}' si riduce al solo ramo else.\n");
    freeAST(root);

    /* PASS 4: while(0) rimosso interamente */
    root = parseAndOptimize(
        "int main() { int a; a = 1; while (0) { a = a + 1; } return a; }");
    body = lastFuncBody(root);
    if (countOfKind(body, ND_WHILE) != 0) {
        fprintf(stderr, "PASS 4 FALLITO: il while(0) non e' stato rimosso\n");
        return 1;
    }
    printf("PASS 4 ok: 'while(0){...}' viene rimosso interamente.\n");
    freeAST(root);

    /* PASS 5: controllo effetti collaterali — f(5)*0 NON viene foldato:
       la chiamata deve sopravvivere anche se il risultato e' moltiplicato per 0 */
    root = parseAndOptimize(
        "int f(int n) { return n; }\n"
        "int main() { int x; x = f(5) * 0; return x; }");
    body = lastFuncBody(root);
    if (countOfKind(body, ND_CALL) != 1) {
        fprintf(stderr, "PASS 5 FALLITO: 'f(5)' e' stata eliminata insieme a '*0'\n");
        return 1;
    }
    printf("PASS 5 ok: 'f(5) * 0' NON elide la chiamata (ha un effetto collaterale).\n");
    freeAST(root);

    /* PASS 6: contro-prova del PASS 5 — y*0 SENZA effetti collaterali fold a 0 */
    root = parseAndOptimize(
        "int main() { int x; int y; y = 5; x = y * 0; return x; }");
    body = lastFuncBody(root);
    assigns[0] = NULL; nAssigns = 0;
    collectOfKind(body, ND_ASSIGN, assigns, 8, &nAssigns);
    if (nAssigns < 2 || assigns[1]->children[1]->kind != ND_NUM_INT ||
        strcmp(assigns[1]->children[1]->text, "0") != 0) {
        fprintf(stderr, "PASS 6 FALLITO: 'y * 0' senza effetti collaterali doveva foldare a 0\n");
        return 1;
    }
    printf("PASS 6 ok: 'y * 0' (senza effetti collaterali) viene foldato in '0'.\n");
    freeAST(root);

    /* PASS 7: doppia negazione unaria costante: -(-5) -> 5 */
    root = parseAndOptimize(
        "int main() { int x; x = -(-5); return x; }");
    body = lastFuncBody(root);
    node = findFirstOfKind(body, ND_ASSIGN);
    if (!node || node->children[1]->kind != ND_NUM_INT ||
        strcmp(node->children[1]->text, "5") != 0) {
        fprintf(stderr, "PASS 7 FALLITO\n");
        return 1;
    }
    printf("PASS 7 ok: '-(-5)' viene foldato ricorsivamente in '5'.\n");
    freeAST(root);

    /* PASS 8: divisione per costante zero NON viene foldata (lasciata al runtime) */
    root = parseAndOptimize(
        "int main() { int x; x = 5 / 0; return x; }");
    body = lastFuncBody(root);
    node = findFirstOfKind(body, ND_ASSIGN);
    if (!node || node->children[1]->kind != ND_BINOP) {
        fprintf(stderr, "PASS 8 FALLITO: '5 / 0' non doveva essere foldato\n");
        return 1;
    }
    printf("PASS 8 ok: '5 / 0' resta un ND_BINOP (non foldato a un valore inventato).\n");
    freeAST(root);

    /* PASS 9: tree height balancing - 'a+b+c+d' e' scritto come catena a
       sinistra dal parser (((a+b)+c)+d, altezza 3); dopo il bilanciamento
       deve diventare (a+b)+(c+d), altezza 2, senza perdere ne' duplicare
       nessuna delle quattro foglie. */
    root = parseAndOptimize(
        "int main() { int a; int b; int c; int d; int x; x = a+b+c+d; return x; }");
    body = lastFuncBody(root);
    node = findFirstOfKind(body, ND_ASSIGN)->children[1];
    if (binopHeight(node) != 2) {
        fprintf(stderr, "PASS 9 FALLITO: altezza attesa 2, trovata %d\n", binopHeight(node));
        return 1;
    }
    if (countOfKind(node, ND_ID) != 4 || countOfKind(node, ND_BINOP) != 3) {
        fprintf(stderr, "PASS 9 FALLITO: foglie/operatori non coerenti dopo il bilanciamento\n");
        return 1;
    }
    printf("PASS 9 ok: 'a+b+c+d' ribilanciato da altezza 3 a 2, tutte le 4 foglie presenti.\n");
    freeAST(root);

    /* PASS 10: la guardia sui float blocca il bilanciamento - un
       letterale float visibile OVUNQUE nella catena (anche non come
       foglia diretta della catena "+" piu' esterna) la lascia intatta,
       altezza 3 invariata (nessun bilanciamento tentato). */
    root = parseAndOptimize(
        "float main() { float a; float b; float c; float x; x = a+1.5+b+c; return x; }");
    body = lastFuncBody(root);
    node = findFirstOfKind(body, ND_ASSIGN)->children[1];
    if (binopHeight(node) != 3) {
        fprintf(stderr, "PASS 10 FALLITO: la catena con un float visibile e' stata bilanciata comunque (altezza %d, attesa 3)\n", binopHeight(node));
        return 1;
    }
    printf("PASS 10 ok: catena con un letterale float visibile NON viene bilanciata (limite noto, rispettato).\n");
    freeAST(root);

    /* PASS 11: numero dispari di foglie (5) - nessuna foglia deve andare
       persa o duplicata, e l'altezza deve comunque scendere sotto quella
       della catena naive (4, per 4 operatori in fila). */
    root = parseAndOptimize(
        "int main() { int a; int b; int c; int d; int e; int x; x = a+b+c+d+e; return x; }");
    body = lastFuncBody(root);
    node = findFirstOfKind(body, ND_ASSIGN)->children[1];
    if (countOfKind(node, ND_ID) != 5 || countOfKind(node, ND_BINOP) != 4) {
        fprintf(stderr, "PASS 11 FALLITO: foglie/operatori non coerenti con 5 addendi\n");
        return 1;
    }
    if (binopHeight(node) >= 4) {
        fprintf(stderr, "PASS 11 FALLITO: altezza non ridotta rispetto alla catena naive (trovata %d)\n", binopHeight(node));
        return 1;
    }
    printf("PASS 11 ok: 5 addendi, nessuna foglia persa/duplicata, altezza ridotta a %d (naive: 4).\n", binopHeight(node));
    freeAST(root);

    printf("\nTutti i test sono passati. Cleanup completato senza errori.\n");
    remove("/tmp/miniC_test_optimize_src.c");
    return 0;
}