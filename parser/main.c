#include <stdio.h>
#include <string.h>
#include "../lexer.h"
#include "error.h"
#include "ast.h"
#include "parser.h"
#include "../symbol_table.h"
#include "../ast_to_symtab.h"
#include "../semantic.h"
#include "../optimize.h"
#include "../ir.h"
#include "../svn.h"
#include "../instr_selector.h"
#include "../sched.h"

static void usage(const char *prog) {
    fprintf(stderr, "Uso: %s <file_sorgente.c> [-S] [-d] [-o <output>]\n", prog);
    fprintf(stderr, "  -S          emette assembly x86-64 AT&T invece dell'IR\n");
    fprintf(stderr, "  -d          debug: stampa asm prima e dopo lo scheduling\n");
    fprintf(stderr, "  -o <file>   scrive l'output su file (default: stdout)\n");
}

int main(int argc, char **argv) {
    const char *src_path = NULL;
    const char *out_path = NULL;
    int emit_asm  = 0;
    int debug     = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-S") == 0) {
            emit_asm = 1;
        } else if (strcmp(argv[i], "-d") == 0) {
            emit_asm = 1;   /* -d implica -S */
            debug    = 1;
        } else if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Errore: -o richiede un argomento\n");
                return 1;
            }
            out_path = argv[++i];
        } else if (argv[i][0] != '-') {
            src_path = argv[i];
        } else {
            fprintf(stderr, "Opzione non riconosciuta: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (!src_path) {
        usage(argv[0]);
        return 1;
    }

    /* ---- Parsing ---- */
    lexer_open(src_path);
    ASTNode *root = ParseProgram();
    lexer_close();

    if (!emit_asm) {
        printf("=== PARSE TREE ===\n");
        printAST(root, 0);
    }

    int parseErrors = totalErrorCount();
    if (parseErrors > 0) {
        printf("\nParsing completato con %d error%s.\n",
               parseErrors, parseErrors == 1 ? "e" : "i");
        freeAST(root);
        return 1;
    }
    if (!emit_asm)
        printf("\nParsing completato con successo.\n");

    /* ---- Analisi semantica ---- */
    Scope *global   = scope_create(NULL);
    int pass1Errors = symtab_populate_globals(root, global);
    int semErrors   = semantic_check(root, global);

    if (!emit_asm) {
        printf("\n=== ANALISI SEMANTICA ===\n");
        printf("Pass 1 (signature globali): %d error%s.\n",
               pass1Errors, pass1Errors == 1 ? "e" : "i");
        printf("Pass 2 + analisi semantica: %d error%s.\n",
               semErrors, semErrors == 1 ? "e" : "i");
    }

    int totalErrors = pass1Errors + semErrors;
    if (totalErrors > 0) {
        if (!emit_asm)
            printf("\nErrori presenti: l'IR non viene generato.\n");
        else
            fprintf(stderr, "Errori semantici (%d): assembly non generato.\n",
                    totalErrors);
        symtab_destroy_tree(global);
        freeAST(root);
        return 1;
    }

    /* ---- Ottimizzazioni AST + generazione IR ---- */
    optimize_ast(root);
    IRProgram *ir = ir_generate(root);

    /* ---- Output: IR oppure assembly ---- */
    if (emit_asm) {
        FILE *out = stdout;
        if (out_path) {
            out = fopen(out_path, "w");
            if (!out) {
                perror(out_path);
                ir_free(ir);
                symtab_destroy_tree(global);
                freeAST(root);
                return 1;
            }
        }

        MachProgram *mp = isel_select(ir);

        if (debug) {
            fprintf(out, "# ======================================================\n");
            fprintf(out, "# ASM DOPO INSTRUCTION SELECTOR (pre-scheduling)\n");
            fprintf(out, "# ======================================================\n");
            isel_emit_asm(mp, out);
            fprintf(out, "\n");
        }

        sched_schedule(mp);

        if (debug) {
            fprintf(out, "# ======================================================\n");
            fprintf(out, "# ASM DOPO INSTRUCTION SCHEDULER (post-scheduling)\n");
            fprintf(out, "# ======================================================\n");
        }
        isel_emit_asm(mp, out);

        mach_free(mp);
        if (out_path) fclose(out);

    } else {
        printf("\n=== IR LINEARE (three-address code) ===\n");
        ir_print(ir);
    }

    ir_free(ir);
    symtab_destroy_tree(global);
    freeAST(root);
    return 0;
}