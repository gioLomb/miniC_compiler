#include <stdio.h>
#include <stdlib.h>
#include "lexer.h"
#include "parser/errorCollector.h"
#include "parser/ast.h"
#include "parser/parser.h"
#include "arena.h"
#include "symbol_table.h"
#include "ast_to_symtab.h"
#include "semantic.h"
#include "ir.h"

static IRProgram *pipeline(const char *src, ASTNode **outRoot, Arena **outArena) {
    const char *path = "/tmp/miniC_test_svn_src.c";
    FILE *f = fopen(path, "w");
    fputs(src, f);
    fclose(f);

    lexer_open(path);
    Arena   *astArena = arena_create(0);
    ASTNode *root     = ParseProgram(astArena);
    lexer_close();

    Scope *global = sym_scopeCreate(NULL);
    st_resolve_global_namespace(root, global);
    int errs = semantic_check(root, global);
    sym_finalize(global);

    if (errs > 0) {
        fprintf(stderr, "semantic errors (%d)\n", errs);
        arena_destroy(astArena);
        exit(1);
    }
    *outRoot  = root;
    *outArena = astArena;
    return ir_generate(root);
}

static IRFunction *lastFunc(IRProgram *prog) {
    return prog->functions[prog->count - 1];
}

static int countOp(IRFunction *f, IROp op) {
    int c = 0;
    for (int i = 0; i < f->count; i++)
        if (f->instrs[i].op == op) c++;
    return c;
}

#define CLEANUP(prog, root, arena) do { ir_free(prog); arena_destroy(arena); } while (0)

int main(void) {
    IRProgram *prog;
    ASTNode *root;
    IRFunction *f;
    Arena *arena;

    /* Common subexpression in while condition */
    prog = pipeline(
        "int main() { int a; int b; int c; "
        "while (a+b > 0 && a+b < 10) { c = c+1; } return c; }",
        &root, &arena);
    f = lastFunc(prog);
    if (countOp(f, IR_ADD) != 2) {
        fprintf(stderr, "PASS 1 FAILED: expected 2 IR_ADD, found %d\n", countOp(f, IR_ADD));
        CLEANUP(prog, root, arena);
        return 1;
    }
    int copies = 0;
    for (int i = 0; i < f->count; i++) {
        IRInstr *in = &f->instrs[i];
        if (in->op == IR_ASSIGN && in->dst.kind == OPND_TEMP && in->src1.kind == OPND_TEMP)
            copies++;
    }
    if (copies != 1) {
        fprintf(stderr, "PASS 1 FAILED: expected 1 temp->temp copy, found %d\n", copies);
        CLEANUP(prog, root, arena);
        return 1;
    }
    printf("PASS 1 ok: 'a+b' in while condition optimized (2 IR_ADD, 1 temp copy).\n");
    CLEANUP(prog, root, arena);

    /* Expressions in different if/else branches stay distinct */
    prog = pipeline(
        "int main() { int a; int b; int x; int y; "
        "if (a > 0) { x = a+b; } else { y = a+b; } return x; }",
        &root, &arena);
    f = lastFunc(prog);
    if (countOp(f, IR_ADD) != 2) {
        fprintf(stderr, "PASS 2 FAILED: the two 'a+b' must not be unified (found %d)\n",
                countOp(f, IR_ADD));
        CLEANUP(prog, root, arena);
        return 1;
    }
    printf("PASS 2 ok: the two 'a+b' in if/else branches remain distinct.\n");
    CLEANUP(prog, root, arena);

    /* Leader invalidated, alias reused */
    prog = pipeline(
        "int main() { int a; int b; int p; int x; int y; "
        "p = a+b; x = p; p = 999; y = a+b; return y; }",
        &root, &arena);
    f = lastFunc(prog);
    if (countOp(f, IR_ADD) != 1) {
        fprintf(stderr, "PASS 3 FAILED: expected 1 IR_ADD, found %d\n", countOp(f, IR_ADD));
        CLEANUP(prog, root, arena);
        return 1;
    }
    printf("PASS 3 ok: primary leader invalidated, alias 'x' reused.\n");
    CLEANUP(prog, root, arena);

    /* Array loads are not unified across a store (no alias analysis) */
    prog = pipeline(
        "int main() { int arr[10]; int other[10]; int i; int t1; int t2; "
        "t1 = arr[i]; other[0] = 5; t2 = arr[i]; return t1+t2; }",
        &root, &arena);
    f = lastFunc(prog);
    if (countOp(f, IR_LOAD_ARR) != 2) {
        fprintf(stderr, "PASS 4 FAILED: two loads not unified (found %d)\n",
                countOp(f, IR_LOAD_ARR));
        CLEANUP(prog, root, arena);
        return 1;
    }
    printf("PASS 4 ok: two loads of 'arr[i]' remain distinct.\n");
    CLEANUP(prog, root, arena);

    printf("\nAll SVN tests passed.\n");
    remove("/tmp/miniC_test_svn_src.c");
    return 0;
}
