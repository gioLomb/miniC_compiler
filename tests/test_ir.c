#include <stdio.h>
#include <stdlib.h>
#include "../lexer.h"
#include "../parser/error.h"
#include "../parser/ast.h"
#include "../parser/parser.h"
#include "../arena.h"
#include "../symbol_table.h"
#include "../ast_to_symtab.h"
#include "../semantic.h"
#include "../ir.h"

static IRProgram *parseAndGenerateIR(const char *src,
                                      ASTNode **outRoot,
                                      Arena   **outArena) {
    const char *path = "/tmp/miniC_test_ir_src.c";
    FILE *f = fopen(path, "w");
    if (!f) { perror("fopen"); exit(1); }
    fputs(src, f);
    fclose(f);

    lexer_open(path);
    Arena   *astArena = arena_create(0);
    ASTNode *root     = ParseProgram(astArena);
    lexer_close();

    Scope *global = sym_scopeCreate(NULL);
    st_resolveGlobalNamespace(root, global);
    int errs = semantic_check(root, global);
    sym_finalize(global);

    if (errs > 0) {
        fprintf(stderr, "errori semantici inattesi (%d) in:\n%s\n", errs, src);
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
    for (int i = 0; i < f->count; i++) if (f->instrs[i].op == op) c++;
    return c;
}

static int firstIndexOfOp(IRFunction *f, IROp op) {
    for (int i = 0; i < f->count; i++) if (f->instrs[i].op == op) return i;
    return -1;
}

#define CLEANUP(prog, root, arena) do { ir_free(prog); freeAST(root); arena_destroy(arena); } while(0)

int main(void) {
    IRProgram *prog;
    ASTNode   *root;
    Arena     *arena;
    IRFunction *f;

    /* PASS 1 */
    prog = parseAndGenerateIR(
        "int main() { int x; x = 1; { int x; x = 2; } return x; }",
        &root, &arena);
    f = lastFunc(prog);
    if (f->count != 3 ||
        f->instrs[0].op != IR_ASSIGN || f->instrs[0].dst.kind != OPND_VAR ||
        f->instrs[1].op != IR_ASSIGN || f->instrs[1].dst.kind != OPND_VAR ||
        f->instrs[2].op != IR_RETURN) {
        fprintf(stderr, "PASS 1 FALLITO: struttura instr inattesa\n"); return 1;
    }
    if (f->instrs[0].dst.data.varLevel == f->instrs[1].dst.data.varLevel) {
        fprintf(stderr, "PASS 1 FALLITO: shadowing non risolto\n"); return 1;
    }
    if (f->instrs[2].src1.data.varLevel != f->instrs[0].dst.data.varLevel ||
        f->instrs[2].src1.data.varOffset != f->instrs[0].dst.data.varOffset) {
        fprintf(stderr, "PASS 1 FALLITO: 'return x' non risolve sulla x esterna\n"); return 1;
    }
    printf("PASS 1 ok: shadowing risolto correttamente.\n");
    CLEANUP(prog, root, arena);

    /* PASS 2 */
    prog = parseAndGenerateIR(
        "int main() { int a; int b; int c; a = 1; b = 2; c = a + b * 2; return c; }",
        &root, &arena);
    f = lastFunc(prog);
    int mulIdx = firstIndexOfOp(f, IR_MUL);
    int addIdx = firstIndexOfOp(f, IR_ADD);
    if (mulIdx < 0 || addIdx < 0 || mulIdx > addIdx) {
        fprintf(stderr, "PASS 2 FALLITO: moltiplicazione non precede addizione\n"); return 1;
    }
    printf("PASS 2 ok: precedenza rispettata.\n");
    CLEANUP(prog, root, arena);

    /* PASS 3 */
    prog = parseAndGenerateIR(
        "int main() { int a; if (a) { a = 1; } else { a = 2; } return a; }",
        &root, &arena);
    f = lastFunc(prog);
    if (countOp(f, IR_IF_FALSE) != 1 || countOp(f, IR_GOTO) != 1 || countOp(f, IR_LABEL) != 2) {
        fprintf(stderr, "PASS 3 FALLITO\n"); return 1;
    }
    printf("PASS 3 ok: if/else genera 1 IF_FALSE, 1 GOTO, 2 LABEL.\n");
    CLEANUP(prog, root, arena);

    /* PASS 4 */
    prog = parseAndGenerateIR(
        "int main() { int a; if (a) { a = 1; } return a; }",
        &root, &arena);
    f = lastFunc(prog);
    if (countOp(f, IR_IF_FALSE) != 1 || countOp(f, IR_GOTO) != 0 || countOp(f, IR_LABEL) != 1) {
        fprintf(stderr, "PASS 4 FALLITO\n"); return 1;
    }
    printf("PASS 4 ok: if senza else genera 1 IF_FALSE, 0 GOTO, 1 LABEL.\n");
    CLEANUP(prog, root, arena);

    /* PASS 5 */
    prog = parseAndGenerateIR(
        "int main() { int a; while (a) { a = a + 1; } return a; }",
        &root, &arena);
    f = lastFunc(prog);
    if (countOp(f, IR_LABEL) != 2 || countOp(f, IR_IF_FALSE) != 1 || countOp(f, IR_GOTO) != 1) {
        fprintf(stderr, "PASS 5 FALLITO\n"); return 1;
    }
    if (f->instrs[0].op != IR_LABEL) {
        fprintf(stderr, "PASS 5 FALLITO: ciclo non inizia con etichetta\n"); return 1;
    }
    printf("PASS 5 ok: while genera testa/coda con salti coerenti.\n");
    CLEANUP(prog, root, arena);

    /* PASS 6 */
    prog = parseAndGenerateIR(
        "int main() { int r; int a; int b; r = a && b; return r; }",
        &root, &arena);
    f = lastFunc(prog);
    if (countOp(f, IR_IF_FALSE) != 2 || countOp(f, IR_GOTO) != 1) {
        fprintf(stderr, "PASS 6 FALLITO\n"); return 1;
    }
    printf("PASS 6 ok: '&&' genera 2 IF_FALSE, 1 GOTO.\n");
    CLEANUP(prog, root, arena);

    /* PASS 7 */
    prog = parseAndGenerateIR(
        "int main() { int r; int a; int b; r = a || b; return r; }",
        &root, &arena);
    f = lastFunc(prog);
    if (countOp(f, IR_IF_FALSE) != 2 || countOp(f, IR_GOTO) != 2) {
        fprintf(stderr, "PASS 7 FALLITO\n"); return 1;
    }
    printf("PASS 7 ok: '||' genera 2 IF_FALSE, 2 GOTO.\n");
    CLEANUP(prog, root, arena);

    /* PASS 8 */
    prog = parseAndGenerateIR(
        "int main() { int v[3]; v[0] = 1; return v[1]; }",
        &root, &arena);
    f = lastFunc(prog);
    if (countOp(f, IR_STORE_ARR) != 1 || countOp(f, IR_LOAD_ARR) != 1) {
        fprintf(stderr, "PASS 8 FALLITO\n"); return 1;
    }
    printf("PASS 8 ok: STORE_ARR/LOAD_ARR generati.\n");
    CLEANUP(prog, root, arena);

    /* PASS 9 */
    prog = parseAndGenerateIR(
        "int f(int n) { return n; }\n"
        "int main() { int x; x = f(5); return x; }",
        &root, &arena);
    f = lastFunc(prog);
    if (countOp(f, IR_PARAM) != 1 || countOp(f, IR_CALL) != 1) {
        fprintf(stderr, "PASS 9 FALLITO\n"); return 1;
    }
    int callIdx = firstIndexOfOp(f, IR_CALL);
    if (f->instrs[callIdx].src2.kind != OPND_CONST_INT ||
        f->instrs[callIdx].src2.data.intVal != 1) {
        fprintf(stderr, "PASS 9 FALLITO: nArgs errato\n"); return 1;
    }
    printf("PASS 9 ok: 'f(5)' genera 1 PARAM, CALL nArgs=1.\n");
    CLEANUP(prog, root, arena);

    /* PASS 10 */
    prog = parseAndGenerateIR(
        "int main() { int i; int n; int f; while (i <= n && f != 0) { i = i + 1; } return i; }",
        &root, &arena);
    f = lastFunc(prog);
    if (countOp(f, IR_IF_FALSE) != 2 || countOp(f, IR_GOTO) != 1 || countOp(f, IR_LABEL) != 2) {
        fprintf(stderr, "PASS 10 FALLITO\n"); return 1;
    }
    printf("PASS 10 ok: 'while (a && b)' salta direttamente all'uscita.\n");
    CLEANUP(prog, root, arena);

    /* PASS 11 */
    prog = parseAndGenerateIR(
        "int main() { int a; int b; int r; if (a || b) { r = 1; } return r; }",
        &root, &arena);
    f = lastFunc(prog);
    if (countOp(f, IR_IF_FALSE) != 2) {
        fprintf(stderr, "PASS 11 FALLITO\n"); return 1;
    }
    printf("PASS 11 ok: 'if (a || b)' salta direttamente.\n");
    CLEANUP(prog, root, arena);

    /* PASS 12 */
    prog = parseAndGenerateIR(
        "int main() { int a; int b; int c; int cane; cane = a+b+c; return cane; }",
        &root, &arena);
    f = lastFunc(prog);
    int lastAddIdx = -1;
    for (int i = 0; i < f->count; i++) if (f->instrs[i].op == IR_ADD) lastAddIdx = i;
    if (lastAddIdx < 0 || f->instrs[lastAddIdx].dst.kind != OPND_VAR) {
        fprintf(stderr, "PASS 12 FALLITO: ultima ADD non scrive in variabile\n"); return 1;
    }
    for (int i = lastAddIdx + 1; i < f->count; i++) {
        if (f->instrs[i].op == IR_ASSIGN && f->instrs[i].src1.kind == OPND_TEMP) {
            fprintf(stderr, "PASS 12 FALLITO: copia ridondante temp->var\n"); return 1;
        }
    }
    printf("PASS 12 ok: 'cane = a+b+c' scrive direttamente nella variabile.\n");
    CLEANUP(prog, root, arena);

    printf("\nTutti i test sono passati.\n");
    remove("/tmp/miniC_test_ir_src.c");
    return 0;
}