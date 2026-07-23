#include <stdio.h>
#include <stdlib.h>
#include "lexer.h"
#include "parser/error.h"
#include "parser/ast.h"
#include "parser/parser.h"
#include "symbol_table.h"
#include "ast_to_symtab.h"
#include "semantic.h"
#include "ir.h"

static IRProgram *pipeline(const char *src, ASTNode **outRoot) {
    const char *path = "/tmp/miniC_test_svn_src.c";
    FILE *f = fopen(path, "w"); fputs(src, f); fclose(f);
    lexer_open(path);
    ASTNode *root = ParseProgram();
    lexer_close();
    Scope *global = scope_create(NULL);
    symtab_populate_globals(root, global);
    int errs = semantic_check(root, global);
    symtab_destroy_tree(global);
    if (errs > 0) { fprintf(stderr, "errori semantici (%d)\n", errs); freeAST(root); exit(1); }
    *outRoot = root;
    return ir_generate(root);
}

static IRFunction *lastFunc(IRProgram *prog) { return prog->functions[prog->count-1]; }

static int countOp(IRFunction *f, IROp op) {
    int c = 0;
    for (int i = 0; i < f->count; i++) if (f->instrs[i].op == op) c++;
    return c;
}

int main(void) {
    IRProgram *prog; ASTNode *root; IRFunction *f;

    /* PASS 1: 'a+b' calcolato due volte nella condizione while con &&.
     * B0(predCount=2)→B1(predCount=1): EBB valida, SVN deve eliminare la seconda.
     * IR atteso: 2 IR_ADD totali (a+b in B0, c+1 nel corpo), 1 copia temp→temp. */
    prog = pipeline(
        "int main() { int a; int b; int c; "
        "while (a+b > 0 && a+b < 10) { c = c+1; } return c; }", &root);
    f = lastFunc(prog);
    if (countOp(f, IR_ADD) != 2) {
        fprintf(stderr, "PASS 1 FALLITO: attese 2 IR_ADD (a+b + c+1), trovate %d\n", countOp(f,IR_ADD));
        ir_free(prog); freeAST(root); return 1;
    }
    int copies = 0;
    for (int i = 0; i < f->count; i++) {
        IRInstr *in = &f->instrs[i];
        if (in->op == IR_ASSIGN && in->dst.kind == OPND_TEMP && in->src1.kind == OPND_TEMP) copies++;
    }
    if (copies != 1) {
        fprintf(stderr, "PASS 1 FALLITO: attesa 1 copia temp->temp, trovate %d\n", copies);
        ir_free(prog); freeAST(root); return 1;
    }
    printf("PASS 1 ok: 'a+b' nella condizione while ottimizzato (2 IR_ADD, 1 copia temp).\n");
    ir_free(prog); freeAST(root);

    /* PASS 2: NON unifica attraverso un punto di confluenza (if/else). */
    prog = pipeline(
        "int main() { int a; int b; int x; int y; "
        "if (a > 0) { x = a+b; } else { y = a+b; } return x; }", &root);
    f = lastFunc(prog);
    if (countOp(f, IR_ADD) != 2) {
        fprintf(stderr, "PASS 2 FALLITO: i due 'a+b' in rami alternativi non devono essere unificati (trovate %d)\n", countOp(f,IR_ADD));
        ir_free(prog); freeAST(root); return 1;
    }
    printf("PASS 2 ok: i due 'a+b' in rami if/else restano distinti.\n");
    ir_free(prog); freeAST(root);

    /* PASS 3: mappa valori-nomi attiva — alias usato quando leader primario invalidato. */
    prog = pipeline(
        "int main() { int a; int b; int p; int x; int y; "
        "p = a+b; x = p; p = 999; y = a+b; return y; }", &root);
    f = lastFunc(prog);
    if (countOp(f, IR_ADD) != 1) {
        fprintf(stderr, "PASS 3 FALLITO: 'a+b' andava ricalcolato una volta sola (trovate %d IR_ADD)\n", countOp(f,IR_ADD));
        ir_free(prog); freeAST(root); return 1;
    }
    printf("PASS 3 ok: leader primario invalidato, alias 'x' riusato.\n");
    ir_free(prog); freeAST(root);

    /* PASS 4: LOAD_ARR mai memoizzato — due letture restano distinte. */
    prog = pipeline(
        "int main() { int arr[10]; int other[10]; int i; int t1; int t2; "
        "t1 = arr[i]; other[0] = 5; t2 = arr[i]; return t1+t2; }", &root);
    f = lastFunc(prog);
    if (countOp(f, IR_LOAD_ARR) != 2) {
        fprintf(stderr, "PASS 4 FALLITO: due letture di 'arr[i]' non devono essere unificate (trovate %d)\n", countOp(f,IR_LOAD_ARR));
        ir_free(prog); freeAST(root); return 1;
    }
    printf("PASS 4 ok: due letture di 'arr[i]' restano distinte.\n");
    ir_free(prog); freeAST(root);

    printf("\nTutti i test SVN sono passati.\n");
    remove("/tmp/miniC_test_svn_src.c");
    return 0;
}