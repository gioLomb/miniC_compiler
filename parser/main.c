#include <stdio.h>
#include <string.h>
#include "../lexer.h"
#include "error.h"
#include "ast.h"
#include "parser.h"
#include "../arena.h"
#include "../symbol_table.h"
#include "../ast_to_symtab.h"
#include "../semantic.h"
#include "../optimize.h"
#include "../ir.h"
#include "../svn.h"
#include "../instr_selector.h"
#include "../sched.h"
#include "../regalloc.h"

static void usage(const char *prog) {
    fprintf(stderr,
            "Uso: %s <file.c> [-S] [-d] [-o <output>]\n"
            "  -S   emette assembly x86-64 AT&T\n"
            "  -d   debug: stampa asm pre e post scheduling\n"
            "  -o   scrive output su file (default: stdout)\n",
            prog);
}

int main(int argc, char **argv) {
    const char *src_path = NULL, *out_path = NULL;
    int emit_asm = 0, debug = 0;

    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "-S") == 0) { emit_asm = 1; }
        else if (strcmp(argv[i], "-d") == 0) { emit_asm = 1; debug = 1; }
        else if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "Errore: -o richiede argomento\n"); return 1; }
            out_path = argv[++i];
        } else if (argv[i][0] != '-') { src_path = argv[i]; }
        else { fprintf(stderr, "Opzione non riconosciuta: %s\n", argv[i]); usage(argv[0]); return 1; }
    }
    if (!src_path) { usage(argv[0]); return 1; }

    /* ---- Parsing ---- */
    lexer_open(src_path);
    Arena   *astArena = arena_create(0);
    ASTNode *root     = ParseProgram(astArena);
    lexer_close();

    if (!emit_asm) { printf("=== PARSE TREE ===\n"); printAST(root, 0); }

    if (totalErrorCount() > 0) {
        printf("\nParsing completato con %d errori.\n", totalErrorCount());
        freeAST(root); arena_destroy(astArena); return 1;
    }
    if (!emit_asm) printf("\nParsing completato con successo.\n");

    /* ---- Analisi semantica ---- */
    Scope *global    = sym_scopeCreate(NULL);
    int pass1Errors  = st_resolveGlobalNamespace(root, global);
    int semErrors    = semantic_check(root, global);

    if (!emit_asm) {
        printf("\n=== ANALISI SEMANTICA ===\n");
        printf("Pass 1: %d errori.\nPass 2+semantica: %d errori.\n",
               pass1Errors, semErrors);
    }
    if (pass1Errors + semErrors > 0) {
        if (emit_asm) fprintf(stderr, "Errori semantici: assembly non generato.\n");
        sym_finalize(global); freeAST(root); arena_destroy(astArena); return 1;
    }

    /* ---- Ottimizzazioni AST + generazione IR ---- */
    optimize_ast(root, astArena);
    IRProgram *ir = ir_generate(root);

    if (!emit_asm) {
        printf("\n=== IR LINEARE (three-address code) ===\n");
        ir_print(ir);
        ir_free(ir); sym_finalize(global);
        freeAST(root); arena_destroy(astArena);
        return 0;
    }

    /* ---- Backend: isel → sched → regalloc → emit ---- */
    FILE *out = stdout;
    if (out_path) {
        out = fopen(out_path, "w");
        if (!out) {
            perror(out_path);
            ir_free(ir); sym_finalize(global);
            freeAST(root); arena_destroy(astArena); return 1;
        }
    }

    MachProgram *mp = isel_select(ir);
    if (debug) { fprintf(out, "# === PRE-SCHEDULING ===\n"); isel_emit_asm(mp, out); fprintf(out, "\n"); }

    sched_schedule(mp);
    if (debug) { fprintf(out, "# === POST-SCHEDULING / PRE-REGALLOC ===\n"); isel_emit_asm(mp, out); fprintf(out, "\n"); }

    regalloc(mp);
    if (debug) fprintf(out, "# === POST-REGALLOC ===\n");

    isel_emit_asm(mp, out);

    mach_free(mp);
    if (out_path) fclose(out);

    ir_free(ir);
    sym_finalize(global);
    freeAST(root);
    arena_destroy(astArena);
    return 0;
}
