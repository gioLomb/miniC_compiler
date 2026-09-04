#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../lexer.h"
// #include "../parser/error.h"          // RIMOSSO
#include "parser/errorCollector.h"          // AGGIUNTO
#include "../parser/ast.h"
#include "../parser/parser.h"
#include "../symbol_table.h"
#include "../ast_to_symtab.h"
#include "../semantic.h"
#include "../optimize.h"

static ASTNode *parseAndOptimize(const char *src, Arena **outArena) {
    const char *path = "/tmp/miniC_test_optimize_src.c";
    FILE *f = fopen(path, "w");
    if (!f) { perror("fopen"); exit(1); }
    fputs(src, f);
    fclose(f);

    lexer_open(path);
    Arena   *astArena = arena_create(0);
    ASTNode *root     = ParseProgram(astArena);
    lexer_close();

    // (Opzionale) pulizia del pending dopo il parsing
    // ec_clear_pending();

    Scope *global = sym_scopeCreate(NULL);
    st_resolve_global_namespace(root, global);
    semantic_check(root, global);
    sym_finalize(global);

    optimize_ast(root, astArena);

    *outArena = astArena;
    return root;
}


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
    for (int i = 0; i < node->nchildren; i++)
        count += countOfKind(node->children[i], kind);
    return count;
}

static void collectOfKind(ASTNode *node, NodeKind kind,
                           ASTNode **out, int maxOut, int *count) {
    if (!node) return;
    if (node->kind == kind && *count < maxOut) out[(*count)++] = node;
    for (int i = 0; i < node->nchildren; i++)
        collectOfKind(node->children[i], kind, out, maxOut, count);
}

static int binopHeight(ASTNode *node) {
    if (!node || node->kind != ND_BINOP) return 0;
    int l = binopHeight(node->children[0]);
    int r = binopHeight(node->children[1]);
    return 1 + (l > r ? l : r);
}

/* macro per ridurre boilerplate cleanup */
#define CLEANUP(root, arena) do {arena_destroy(arena); } while(0)

int main(void) {
    ASTNode *root, *body, *node;
    Arena   *arena;

    /* PASS 1 */
    root = parseAndOptimize("int main() { int z; z = 3 + 2; return z; }", &arena);
    body = lastFuncBody(root);
    node = findFirstOfKind(body, ND_ASSIGN);
    if (!node || node->children[1]->kind != ND_NUM_INT ||
        strcmp(node->children[1]->text, "5") != 0) {
        fprintf(stderr, "PASS 1 FALLITO\n"); return 1;
    }
    printf("PASS 1 ok: 'z = 3 + 2;' viene foldato in 'z = 5;'.\n");
    CLEANUP(root, arena);

    /* PASS 2 */
    root = parseAndOptimize(
        "int main() { int x; int y; y = 1; x = y + 0; return x; }", &arena);
    body = lastFuncBody(root);
    ASTNode *assigns[8]; int nAssigns = 0;
    collectOfKind(body, ND_ASSIGN, assigns, 8, &nAssigns);
    if (nAssigns < 2 || assigns[1]->children[1]->kind != ND_ID ||
        strcmp(assigns[1]->children[1]->text, "y") != 0) {
        fprintf(stderr, "PASS 2 FALLITO\n"); return 1;
    }
    printf("PASS 2 ok: 'x = y + 0;' viene semplificato in 'x = y;'.\n");
    CLEANUP(root, arena);

    /* PASS 3 */
    root = parseAndOptimize(
        "int main() { int a; if (0) { a = 1; } else { a = 2; } return a; }", &arena);
    body = lastFuncBody(root);
    if (countOfKind(body, ND_IF) != 0) {
        fprintf(stderr, "PASS 3 FALLITO: l'ND_IF non e' stato rimosso\n"); return 1;
    }
    node = findFirstOfKind(body, ND_ASSIGN);
    if (!node || node->children[1]->kind != ND_NUM_INT ||
        strcmp(node->children[1]->text, "2") != 0) {
        fprintf(stderr, "PASS 3 FALLITO: non e' rimasto 'a = 2;'\n"); return 1;
    }
    printf("PASS 3 ok: 'if(0){a=1;}else{a=2;}' si riduce al solo ramo else.\n");
    CLEANUP(root, arena);

    /* PASS 4 */
    root = parseAndOptimize(
        "int main() { int a; a = 1; while (0) { a = a + 1; } return a; }", &arena);
    body = lastFuncBody(root);
    if (countOfKind(body, ND_WHILE) != 0) {
        fprintf(stderr, "PASS 4 FALLITO: il while(0) non e' stato rimosso\n"); return 1;
    }
    printf("PASS 4 ok: 'while(0){...}' viene rimosso interamente.\n");
    CLEANUP(root, arena);

    /* PASS 5 */
    root = parseAndOptimize(
        "int f(int n) { return n; }\n"
        "int main() { int x; x = f(5) * 0; return x; }", &arena);
    body = lastFuncBody(root);
    if (countOfKind(body, ND_CALL) != 1) {
        fprintf(stderr, "PASS 5 FALLITO: 'f(5)' e' stata eliminata insieme a '*0'\n"); return 1;
    }
    printf("PASS 5 ok: 'f(5) * 0' NON elide la chiamata (ha un effetto collaterale).\n");
    CLEANUP(root, arena);

    /* PASS 6 */
    root = parseAndOptimize(
        "int main() { int x; int y; y = 5; x = y * 0; return x; }", &arena);
    body = lastFuncBody(root);
    assigns[0] = NULL; nAssigns = 0;
    collectOfKind(body, ND_ASSIGN, assigns, 8, &nAssigns);
    if (nAssigns < 2 || assigns[1]->children[1]->kind != ND_NUM_INT ||
        strcmp(assigns[1]->children[1]->text, "0") != 0) {
        fprintf(stderr, "PASS 6 FALLITO\n"); return 1;
    }
    printf("PASS 6 ok: 'y * 0' (senza effetti collaterali) viene foldato in '0'.\n");
    CLEANUP(root, arena);

    /* PASS 7 */
    root = parseAndOptimize("int main() { int x; x = -(-5); return x; }", &arena);
    body = lastFuncBody(root);
    node = findFirstOfKind(body, ND_ASSIGN);
    if (!node || node->children[1]->kind != ND_NUM_INT ||
        strcmp(node->children[1]->text, "5") != 0) {
        fprintf(stderr, "PASS 7 FALLITO\n"); return 1;
    }
    printf("PASS 7 ok: '-(-5)' viene foldato ricorsivamente in '5'.\n");
    CLEANUP(root, arena);

    /* PASS 8 */
    root = parseAndOptimize("int main() { int x; x = 5 / 0; return x; }", &arena);
    body = lastFuncBody(root);
    node = findFirstOfKind(body, ND_ASSIGN);
    if (!node || node->children[1]->kind != ND_BINOP) {
        fprintf(stderr, "PASS 8 FALLITO: '5 / 0' non doveva essere foldato\n"); return 1;
    }
    printf("PASS 8 ok: '5 / 0' resta un ND_BINOP (non foldato).\n");
    CLEANUP(root, arena);

    /* PASS 9 */
    root = parseAndOptimize(
        "int main() { int a; int b; int c; int d; int x; x = a+b+c+d; return x; }", &arena);
    body = lastFuncBody(root);
    node = findFirstOfKind(body, ND_ASSIGN)->children[1];
    if (binopHeight(node) != 2) {
        fprintf(stderr, "PASS 9 FALLITO: altezza attesa 2, trovata %d\n", binopHeight(node)); return 1;
    }
    if (countOfKind(node, ND_ID) != 4 || countOfKind(node, ND_BINOP) != 3) {
        fprintf(stderr, "PASS 9 FALLITO: foglie/operatori non coerenti\n"); return 1;
    }
    printf("PASS 9 ok: 'a+b+c+d' ribilanciato da altezza 3 a 2.\n");
    CLEANUP(root, arena);

    /* PASS 10 */
    root = parseAndOptimize(
        "float main() { float a; float b; float c; float x; x = a+1.5+b+c; return x; }", &arena);
    body = lastFuncBody(root);
    node = findFirstOfKind(body, ND_ASSIGN)->children[1];
    if (binopHeight(node) != 3) {
        fprintf(stderr, "PASS 10 FALLITO: altezza %d, attesa 3\n", binopHeight(node)); return 1;
    }
    printf("PASS 10 ok: catena con float NON bilanciata (altezza 3).\n");
    CLEANUP(root, arena);

    /* PASS 11 */
    root = parseAndOptimize(
        "int main() { int a; int b; int c; int d; int e; int x; x = a+b+c+d+e; return x; }", &arena);
    body = lastFuncBody(root);
    node = findFirstOfKind(body, ND_ASSIGN)->children[1];
    if (countOfKind(node, ND_ID) != 5 || countOfKind(node, ND_BINOP) != 4) {
        fprintf(stderr, "PASS 11 FALLITO: foglie/operatori non coerenti\n"); return 1;
    }
    if (binopHeight(node) >= 4) {
        fprintf(stderr, "PASS 11 FALLITO: altezza non ridotta (trovata %d)\n", binopHeight(node)); return 1;
    }
    printf("PASS 11 ok: 5 addendi, altezza ridotta a %d (naive: 4).\n", binopHeight(node));
    CLEANUP(root, arena);

    printf("\nTutti i test sono passati. Cleanup completato senza errori.\n");
    remove("/tmp/miniC_test_optimize_src.c");
    return 0;
}
