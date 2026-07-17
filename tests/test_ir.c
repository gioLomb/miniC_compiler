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

/* Scrive 'src' su un file temporaneo e fa girare l'intera pipeline fino a
   ir_generate incluso (senza optimize_ast: questi test isolano ir.c,
   optimize.c e' gia' testato per conto suo in test_optimize.c).
   Restituisce l'IRProgram e, tramite outRoot, la radice dell'AST (il
   chiamante e' responsabile di ir_free()/freeAST() a fine test). */
static IRProgram *parseAndGenerateIR(const char *src, ASTNode **outRoot) {
    const char *path = "/tmp/miniC_test_ir_src.c";
    FILE *f = fopen(path, "w");
    if (!f) { perror("fopen"); exit(1); }
    fputs(src, f);
    fclose(f);

    lexer_open(path);
    ASTNode *root = ParseProgram();
    lexer_close();

    Scope *global = scope_create(NULL);
    symtab_populate_globals(root, global);
    int errs = semantic_check(root, global);
    symtab_destroy_tree(global);

    if (errs > 0) {
        fprintf(stderr, "pipeline di test: errori semantici inattesi (%d) in:\n%s\n", errs, src);
        exit(1);
    }

    *outRoot = root;
    return ir_generate(root);
}

/* L'ultima funzione tradotta e' quella che ci interessa ispezionare nei
   sorgenti di test qui sotto (in genere main, l'ultima dichiarata). */
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

int main(void) {
    IRProgram *prog;
    ASTNode *root;
    IRFunction *f;

    /* PASS 1: shadowing - le due 'x' devono ricevere identita' diverse,
       e 'return x' deve risolvere sulla x ESTERNA (uscendo dal blocco
       interno), non su quella appena dichiarata nel blocco annidato. */
    prog = parseAndGenerateIR(
        "int main() { int x; x = 1; { int x; x = 2; } return x; }", &root);
    f = lastFunc(prog);
    if (f->count != 3 ||
        f->instrs[0].op != IR_ASSIGN || f->instrs[0].dst.kind != OPND_VAR ||
        f->instrs[1].op != IR_ASSIGN || f->instrs[1].dst.kind != OPND_VAR ||
        f->instrs[2].op != IR_RETURN) {
        fprintf(stderr, "PASS 1 FALLITO: struttura instr inattesa\n");
        return 1;
    }
    if (f->instrs[0].dst.as.var.level == f->instrs[1].dst.as.var.level) {
        fprintf(stderr, "PASS 1 FALLITO: le due 'x' hanno lo stesso scopeLevel (shadowing non risolto)\n");
        return 1;
    }
    if (f->instrs[2].src1.as.var.level != f->instrs[0].dst.as.var.level ||
        f->instrs[2].src1.as.var.offset != f->instrs[0].dst.as.var.offset) {
        fprintf(stderr, "PASS 1 FALLITO: 'return x' non risolve sulla x esterna\n");
        return 1;
    }
    printf("PASS 1 ok: shadowing risolto correttamente ('return x' torna alla x esterna).\n");
    ir_free(prog); freeAST(root);

    /* PASS 2: precedenza aritmetica - 'a + b * 2' deve calcolare prima la
       moltiplicazione, poi usarne il risultato nell'addizione. */
    prog = parseAndGenerateIR(
        "int main() { int a; int b; int c; a = 1; b = 2; c = a + b * 2; return c; }", &root);
    f = lastFunc(prog);
    int mulIdx = firstIndexOfOp(f, IR_MUL);
    int addIdx = firstIndexOfOp(f, IR_ADD);
    if (mulIdx < 0 || addIdx < 0 || mulIdx > addIdx) {
        fprintf(stderr, "PASS 2 FALLITO: la moltiplicazione non precede l'addizione\n");
        return 1;
    }
    printf("PASS 2 ok: precedenza rispettata ('b * 2' calcolato prima di sommarlo ad 'a').\n");
    ir_free(prog); freeAST(root);

    /* PASS 3: if/else - un salto condizionale, un salto incondizionato
       per saltare il ramo else, due etichette (else, fine). */
    prog = parseAndGenerateIR(
        "int main() { int a; if (a) { a = 1; } else { a = 2; } return a; }", &root);
    f = lastFunc(prog);
    if (countOp(f, IR_IF_FALSE) != 1 || countOp(f, IR_GOTO) != 1 || countOp(f, IR_LABEL) != 2) {
        fprintf(stderr, "PASS 3 FALLITO: if/else non genera il pattern atteso\n");
        return 1;
    }
    printf("PASS 3 ok: if/else genera 1 IF_FALSE, 1 GOTO, 2 LABEL.\n");
    ir_free(prog); freeAST(root);

    /* PASS 4: if senza else - nessun salto incondizionato, una sola etichetta. */
    prog = parseAndGenerateIR(
        "int main() { int a; if (a) { a = 1; } return a; }", &root);
    f = lastFunc(prog);
    if (countOp(f, IR_IF_FALSE) != 1 || countOp(f, IR_GOTO) != 0 || countOp(f, IR_LABEL) != 1) {
        fprintf(stderr, "PASS 4 FALLITO: if senza else non genera il pattern atteso\n");
        return 1;
    }
    printf("PASS 4 ok: if senza else genera 1 IF_FALSE, 0 GOTO, 1 LABEL.\n");
    ir_free(prog); freeAST(root);

    /* PASS 5: while - due etichette (inizio, fine), un salto indietro
       all'inizio, un salto condizionale in uscita. */
    prog = parseAndGenerateIR(
        "int main() { int a; while (a) { a = a + 1; } return a; }", &root);
    f = lastFunc(prog);
    if (countOp(f, IR_LABEL) != 2 || countOp(f, IR_IF_FALSE) != 1 || countOp(f, IR_GOTO) != 1) {
        fprintf(stderr, "PASS 5 FALLITO: while non genera il pattern atteso\n");
        return 1;
    }
    if (f->instrs[0].op != IR_LABEL) {
        fprintf(stderr, "PASS 5 FALLITO: il ciclo non inizia con l'etichetta di testa\n");
        return 1;
    }
    printf("PASS 5 ok: while genera testa/coda con salti coerenti.\n");
    ir_free(prog); freeAST(root);

    /* PASS 6: && a corto-circuito - due IF_FALSE (uno per operando), un
       solo GOTO (il ramo "vero" prosegue dritto nel fallthrough). */
    prog = parseAndGenerateIR(
        "int main() { int r; int a; int b; r = a && b; return r; }", &root);
    f = lastFunc(prog);
    if (countOp(f, IR_IF_FALSE) != 2 || countOp(f, IR_GOTO) != 1) {
        fprintf(stderr, "PASS 6 FALLITO: '&&' non genera il pattern di corto-circuito atteso\n");
        return 1;
    }
    printf("PASS 6 ok: '&&' genera 2 IF_FALSE (un salto per operando), 1 GOTO.\n");
    ir_free(prog); freeAST(root);

    /* PASS 7: || a corto-circuito - anche qui due IF_FALSE, ma due GOTO
       (sia il ramo "vero anticipato" sia quello dopo aver valutato il
       secondo operando devono saltare alla fine). */
    prog = parseAndGenerateIR(
        "int main() { int r; int a; int b; r = a || b; return r; }", &root);
    f = lastFunc(prog);
    if (countOp(f, IR_IF_FALSE) != 2 || countOp(f, IR_GOTO) != 2) {
        fprintf(stderr, "PASS 7 FALLITO: '||' non genera il pattern di corto-circuito atteso\n");
        return 1;
    }
    printf("PASS 7 ok: '||' genera 2 IF_FALSE, 2 GOTO.\n");
    ir_free(prog); freeAST(root);

    /* PASS 8: array - una IR_STORE_ARR per l'assegnamento a v[0], una
       IR_LOAD_ARR per la lettura di v[1]. */
    prog = parseAndGenerateIR(
        "int main() { int v[3]; v[0] = 1; return v[1]; }", &root);
    f = lastFunc(prog);
    if (countOp(f, IR_STORE_ARR) != 1 || countOp(f, IR_LOAD_ARR) != 1) {
        fprintf(stderr, "PASS 8 FALLITO: accesso ad array non genera LOAD/STORE_ARR attesi\n");
        return 1;
    }
    printf("PASS 8 ok: 'v[0] = 1' / 'v[1]' generano STORE_ARR/LOAD_ARR.\n");
    ir_free(prog); freeAST(root);

    /* PASS 9: chiamata di funzione - un PARAM per ogni argomento, una
       CALL con il numero di argomenti corretto nel secondo operando. */
    prog = parseAndGenerateIR(
        "int f(int n) { return n; }\n"
        "int main() { int x; x = f(5); return x; }", &root);
    f = lastFunc(prog);   /* main, l'ultima funzione dichiarata */
    if (countOp(f, IR_PARAM) != 1 || countOp(f, IR_CALL) != 1) {
        fprintf(stderr, "PASS 9 FALLITO: 'f(5)' non genera PARAM/CALL attesi\n");
        return 1;
    }
    int callIdx = firstIndexOfOp(f, IR_CALL);
    if (f->instrs[callIdx].src2.kind != OPND_CONST_INT || f->instrs[callIdx].src2.as.intVal != 1) {
        fprintf(stderr, "PASS 9 FALLITO: la CALL non riporta il numero corretto di argomenti\n");
        return 1;
    }
    printf("PASS 9 ok: 'f(5)' genera 1 PARAM e una CALL con nArgs=1.\n");
    ir_free(prog); freeAST(root);

    printf("\nTutti i test sono passati.\n");
    remove("/tmp/miniC_test_ir_src.c");
    return 0;
}
