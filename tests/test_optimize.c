#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../lexer.h"
#include "parser/errorCollector.h"
#include "../parser/ast.h"
#include "../parser/parser.h"
#include "../symbol_table.h"
#include "../ast_to_symtab.h"
#include "../semantic.h"
#include "../ast_optimizer.h"

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

#define CLEANUP(root, arena) do { arena_destroy(arena); } while (0)

int main(void) {
    ASTNode *root, *body, *node;
    Arena   *arena;

    /* Constant folding of addition */
    root = parseAndOptimize("int main() { int z; z = 3 + 2; return z; }", &arena);
    body = lastFuncBody(root);
    node = findFirstOfKind(body, ND_ASSIGN);
    if (!node || node->children[1]->kind != ND_NUM_INT ||
        strcmp(node->children[1]->text, "5") != 0) {
        fprintf(stderr, "PASS 1 FAILED\n");
        return 1;
    }
    printf("PASS 1 ok: 'z = 3 + 2;' folded to 'z = 5;'.\n");
    CLEANUP(root, arena);

    /* Algebraic simplification y + 0 -> y */
    root = parseAndOptimize(
        "int main() { int x; int y; y = 1; x = y + 0; return x; }", &arena);
    body = lastFuncBody(root);
    ASTNode *assigns[8];
    int nAssigns = 0;
    collectOfKind(body, ND_ASSIGN, assigns, 8, &nAssigns);
    if (nAssigns < 2 || assigns[1]->children[1]->kind != ND_ID ||
        strcmp(assigns[1]->children[1]->text, "y") != 0) {
        fprintf(stderr, "PASS 2 FAILED\n");
        return 1;
    }
    printf("PASS 2 ok: 'x = y + 0;' simplified to 'x = y;'.\n");
    CLEANUP(root, arena);

    /*
     * Control-flow DCE (if(0)/while(0)) is intentionally left to CP on the IR
     * (try_fold_if_false).  AST optimizer only does algebraic identities +
     * int chain rebalancing, so the old PASS 3/4 that required ND_IF/ND_WHILE
     * removal are gone.
     */

    /* Call with side effect is not eliminated by *0 */
    root = parseAndOptimize(
        "int f(int n) { return n; }\n"
        "int main() { int x; x = f(5) * 0; return x; }", &arena);
    body = lastFuncBody(root);
    if (countOfKind(body, ND_CALL) != 1) {
        fprintf(stderr, "PASS 3 FAILED: 'f(5)' was eliminated together with '*0'\n");
        return 1;
    }
    printf("PASS 3 ok: 'f(5) * 0' does not eliminate the call (side effect).\n");
    CLEANUP(root, arena);

    /* Pure *0 folds to 0 */
    root = parseAndOptimize(
        "int main() { int x; int y; y = 5; x = y * 0; return x; }", &arena);
    body = lastFuncBody(root);
    assigns[0] = NULL;
    nAssigns = 0;
    collectOfKind(body, ND_ASSIGN, assigns, 8, &nAssigns);
    if (nAssigns < 2 || assigns[1]->children[1]->kind != ND_NUM_INT ||
        strcmp(assigns[1]->children[1]->text, "0") != 0) {
        fprintf(stderr, "PASS 4 FAILED\n");
        return 1;
    }
    printf("PASS 4 ok: 'y * 0' (no side effects) folded to '0'.\n");
    CLEANUP(root, arena);

    /* Double negation folds */
    root = parseAndOptimize("int main() { int x; x = -(-5); return x; }", &arena);
    body = lastFuncBody(root);
    node = findFirstOfKind(body, ND_ASSIGN);
    if (!node || node->children[1]->kind != ND_NUM_INT ||
        strcmp(node->children[1]->text, "5") != 0) {
        fprintf(stderr, "PASS 5 FAILED\n");
        return 1;
    }
    printf("PASS 5 ok: '-(-5)' folded recursively to '5'.\n");
    CLEANUP(root, arena);

    /* Division by zero is not folded */
    root = parseAndOptimize("int main() { int x; x = 5 / 0; return x; }", &arena);
    body = lastFuncBody(root);
    node = findFirstOfKind(body, ND_ASSIGN);
    if (!node || node->children[1]->kind != ND_BINOP) {
        fprintf(stderr, "PASS 6 FAILED: '5 / 0' should not have been folded\n");
        return 1;
    }
    printf("PASS 6 ok: '5 / 0' remains an ND_BINOP (not folded).\n");
    CLEANUP(root, arena);

    /* Associative chain balancing (int) */
    root = parseAndOptimize(
        "int main() { int a; int b; int c; int d; int x; x = a+b+c+d; return x; }", &arena);
    body = lastFuncBody(root);
    node = findFirstOfKind(body, ND_ASSIGN)->children[1];
    if (binopHeight(node) != 2) {
        fprintf(stderr, "PASS 7 FAILED: expected height 2, found %d\n", binopHeight(node));
        return 1;
    }
    if (countOfKind(node, ND_ID) != 4 || countOfKind(node, ND_BINOP) != 3) {
        fprintf(stderr, "PASS 7 FAILED: leaves/operators inconsistent\n");
        return 1;
    }
    printf("PASS 7 ok: 'a+b+c+d' rebalanced from height 3 to 2.\n");
    CLEANUP(root, arena);

    /* Float chains are not balanced */
    root = parseAndOptimize(
        "float main() { float a; float b; float c; float x; x = a+1.5+b+c; return x; }", &arena);
    body = lastFuncBody(root);
    node = findFirstOfKind(body, ND_ASSIGN)->children[1];
    if (binopHeight(node) != 3) {
        fprintf(stderr, "PASS 8 FAILED: height %d, expected 3\n", binopHeight(node));
        return 1;
    }
    printf("PASS 8 ok: float chain NOT balanced (height 3).\n");
    CLEANUP(root, arena);

    /* Longer int chain is balanced */
    root = parseAndOptimize(
        "int main() { int a; int b; int c; int d; int e; int x; x = a+b+c+d+e; return x; }", &arena);
    body = lastFuncBody(root);
    node = findFirstOfKind(body, ND_ASSIGN)->children[1];
    if (countOfKind(node, ND_ID) != 5 || countOfKind(node, ND_BINOP) != 4) {
        fprintf(stderr, "PASS 9 FAILED: leaves/operators inconsistent\n");
        return 1;
    }
    if (binopHeight(node) >= 4) {
        fprintf(stderr, "PASS 9 FAILED: height not reduced (found %d)\n", binopHeight(node));
        return 1;
    }
    printf("PASS 9 ok: 5 addends, height reduced to %d (naive: 4).\n", binopHeight(node));
    CLEANUP(root, arena);

    printf("\nAll tests passed. Cleanup completed without errors.\n");
    remove("/tmp/miniC_test_optimize_src.c");
    return 0;
}
