#include <stdio.h>
#include <stdlib.h>
#include "lexer.h"
#include "parser/error.h"
#include "parser/ast.h"
#include "parser/parser.h"
#include "arena.h"
#include "symbol_table.h"
#include "ast_to_symtab.h"
#include "semantic.h"
#include "ir.h"

static IRProgram *pipeline(const char *src, ASTNode **outRoot, Arena **outArena) {
    const char *path = "/tmp/miniC_test_svn_src.c";
    FILE *f = fopen(path, "w"); fputs(src, f); fclose(f);

    lexer_open(path);
    Arena   *astArena = arena_create(0);
    ASTNode *root     = ParseProgram(astArena);
    lexer_close();

    Scope *global = sym_scopeCreate(NULL);
    st_resolveGlobalNamespace(root, global);
    int errs = semantic_check(root, global);
    sym_finalize(global);

    if (errs > 0) {
        fprintf(stderr, "errori semantici (%d)\n", errs);
        freeAST(root); arena_destroy(astArena); exit(1);
    }
    *outRoot  = root;
    *outArena = astArena;
    return ir_generate(root);
}

static IRFunction *lastFunc(IRProgram *prog) { return prog->functions[prog->count-1]; }

static int countOp(IRFunction *f, IROp op) {
    int c = 0;
    for (int i = 0; i < f->count; i++) if (f->instrs[i].op == op) c++;
    return c;
}

#define CLEANUP(prog, root, arena) do { ir_free(prog); freeAST(root); arena_destroy(arena); } while(0)

int main(void) {
    IRProgram *prog; ASTNode *root; IRFunction *f; Arena *arena;

    /* PASS 1 */
    prog = pipeline(
        "int main() { int a; int b; int c; "
        "while (a+b > 0 && a+b < 10) { c = c+1; } return c; }",
        &root, &arena);
    f = lastFunc(prog);
    if (countOp(f, IR_ADD) != 2) {
        fprintf(stderr, "PASS 1 FALLITO: attese 2 IR_ADD, trovate %d\n", countOp(f,IR_ADD));
        CLEANUP(prog, root, arena); return 1;
    }
    int copies = 0;
    for (int i = 0; i < f->count; i++) {
        IRInstr *in = &f->instrs[i];
        if (in->op == IR_ASSIGN && in->dst.kind == OPND_TEMP && in->src1.kind == OPND_TEMP) copies++;
    }
    if (copies != 1) {
        fprintf(stderr, "PASS 1 FALLITO: attesa 1 copia temp->temp, trovate %d\n", copies);
        CLEANUP(prog, root, arena); return 1;
    }
    printf("PASS 1 ok: 'a+b' nella condizione while ottimizzato (2 IR_ADD, 1 copia temp).\n");
    CLEANUP(prog, root, arena);

    /* PASS 2 */
    prog = pipeline(
        "int main() { int a; int b; int x; int y; "
        "if (a > 0) { x = a+b; } else { y = a+b; } return x; }",
        &root, &arena);
    f = lastFunc(prog);
    if (countOp(f, IR_ADD) != 2) {
        fprintf(stderr, "PASS 2 FALLITO: i due 'a+b' non devono essere unificati (trovate %d)\n", countOp(f,IR_ADD));
        CLEANUP(prog, root, arena); return 1;
    }
    printf("PASS 2 ok: i due 'a+b' in rami if/else restano distinti.\n");
    CLEANUP(prog, root, arena);

    /* PASS 3 */
    prog = pipeline(
        "int main() { int a; int b; int p; int x; int y; "
        "p = a+b; x = p; p = 999; y = a+b; return y; }",
        &root, &arena);
    f = lastFunc(prog);
    if (countOp(f, IR_ADD) != 1) {
        fprintf(stderr, "PASS 3 FALLITO: attesa 1 IR_ADD, trovate %d\n", countOp(f,IR_ADD));
        CLEANUP(prog, root, arena); return 1;
    }
    printf("PASS 3 ok: leader primario invalidato, alias 'x' riusato.\n");
    CLEANUP(prog, root, arena);

    /* PASS 4 */
    prog = pipeline(
        "int main() { int arr[10]; int other[10]; int i; int t1; int t2; "
        "t1 = arr[i]; other[0] = 5; t2 = arr[i]; return t1+t2; }",
        &root, &arena);
    f = lastFunc(prog);
    if (countOp(f, IR_LOAD_ARR) != 2) {
        fprintf(stderr, "PASS 4 FALLITO: due letture non unificate (trovate %d)\n", countOp(f,IR_LOAD_ARR));
        CLEANUP(prog, root, arena); return 1;
    }
    printf("PASS 4 ok: due letture di 'arr[i]' restano distinte.\n");
    CLEANUP(prog, root, arena);

    printf("\nTutti i test SVN sono passati.\n");
    remove("/tmp/miniC_test_svn_src.c");
    return 0;
}