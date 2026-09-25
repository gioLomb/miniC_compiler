/**
 * @file ir_global.c
 * @brief Global variable registration for IR programs.
 *
 * Split from ir.c; pure data-side of IR generation (no function bodies).
 */

#include "ir.h"
#include "parser/ast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Parsed view of a global declaration's textual encoding.
 *
 * tyName/name point into the caller-owned mutable buffer passed to
 * ir_parse_global_decl_text, so they stay valid only as long as that
 * buffer does.
 */
typedef struct {
    char *tyName;
    char *name;
    int   isArray;
    int   arraySize;
} GlobalDeclInfo;

/**
 * @brief Parse buf ("type name" or "type name[size]") in place into its
 *        type/name/array-size components (same convention as ast_to_symtab.c).
 *
 * @param buf  Mutable copy of decl->text; NUL bytes are inserted at the
 *             type/name and name/'[' boundaries, so out->tyName/out->name
 *             end up pointing into it.
 * @return 1 on success, 0 if the text is malformed (no space separator).
 */
static int ir_parse_global_decl_text(char *buf, GlobalDeclInfo *out) {
    char *space = strchr(buf, ' ');
    if (!space) return 0; // malformed text: caller skips defensively
    *space      = '\0';
    out->tyName = buf;
    char *rest  = space + 1;

    out->isArray   = 0;
    out->arraySize = 0;
    char *bracket = strchr(rest, '[');
    if (bracket) {
        *bracket       = '\0';
        out->name      = rest;
        out->isArray   = 1;
        out->arraySize = atoi(bracket + 1);
    } else {
        out->name = rest;
    }
    return 1;
}

static inline DataType ir_global_data_type(const char *tyName) {
    if      (tyName[0] == 'i') return T_INT;
    else if (tyName[0] == 'f') return T_FLOAT;
    return T_VOID;
}

/**
 * @brief Grow prog->globals if needed (standard doubling growth) and
 *        return a pointer to the freshly appended slot.
 */
static IRGlobalVar *ir_globals_append_slot(IRProgram *prog) {
    if (prog->globalCount == prog->globalCap) {
        prog->globalCap = prog->globalCap ? prog->globalCap * 2 : 8;
        prog->globals   = realloc(prog->globals,
                                  (size_t)prog->globalCap * sizeof(IRGlobalVar));
    }
    return &prog->globals[prog->globalCount++];
}

/**
 * @brief Pre-evaluate a global's constant initializer list into a flat
 *        array of longs (int value, or float re-interpreted as raw bits,
 *        so a single 'long' array can hold both int and float initializers).
 *
 * When the global is float and the initializer is an int literal (widening
 * allowed by the language), the int is converted to float and the IEEE bit
 * pattern is stored — not the integer value.  Non-literal initializers are
 * rejected by the semantic pass; here they still default to 0 defensively.
 *
 * @param decl  ND_VAR_DECL node whose children (if any) are the initializers.
 * @param gv    Global var to populate (initVals/initCount left untouched
 *              if decl has no children).
 */
static void ir_build_global_init_vals(ASTNode *decl, IRGlobalVar *gv) {
    if (decl->nchildren == 0) return; // no initializer: nothing to build

    int cnt       = decl->nchildren;
    gv->initVals  = malloc((size_t)cnt * sizeof(long));
    gv->initCount = cnt;
    for (int j = 0; j < cnt; j++) {
        ASTNode *ch = decl->children[j];
        if (gv->dataType == T_FLOAT) {
            /* Always store IEEE-754 bit pattern for float globals. */
            float fv = 0.0f;
            if (ch->kind == ND_NUM_FLOAT)
                fv = (float)atof(ch->text);
            else if (ch->kind == ND_NUM_INT)
                fv = (float)atol(ch->text); /* int → float widening */
            long lv = 0;
            memcpy(&lv, &fv, sizeof fv);
            gv->initVals[j] = lv;
        } else if (ch->kind == ND_NUM_INT) {
            gv->initVals[j] = atol(ch->text);
        } else if (ch->kind == ND_NUM_FLOAT) {
            /* float literal into int global: truncate (semantic should reject) */
            gv->initVals[j] = (long)(float)atof(ch->text);
        } else {
            gv->initVals[j] = 0; // non-literal: semantic error; defensive default
        }
    }
}

static void ir_add_global(IRProgram *prog, ASTNode *decl, int symOffset) {
    if (!decl || decl->kind != ND_VAR_DECL) return;

    // decl->text encodes "type name" or "type name[size]"; parse it
    // in-place on a mutable copy (same convention as ast_to_symtab.c)
    char *buf = strdup(decl->text);
    GlobalDeclInfo info;
    if (!ir_parse_global_decl_text(buf, &info)) { free(buf); return; } // malformed text: skip defensively

    IRGlobalVar *gv = ir_globals_append_slot(prog);
    gv->name        = strdup(info.name);
    gv->dataType    = ir_global_data_type(info.tyName);
    gv->isArray     = info.isArray;
    gv->arraySize   = info.arraySize;
    gv->symOffset   = symOffset;
    gv->initVals    = NULL;
    gv->initCount   = 0;

    ir_build_global_init_vals(decl, gv);

    free(buf);
}


void ir_register_globals(IRProgram *prog, ASTNode *program) {
    /* Global symOffset is assigned once in st_resolve_global_namespace and
     * stamped on each ND_VAR_DECL / ND_FUNC_DECL.  Reusing decl->offset here
     * keeps a single source of truth (a second counter would diverge if pass1
     * ever continued after a failed bind).  main aborts before ir_generate
     * when pass1Errors > 0, so every VAR_DECL we see has a valid stamp. */
    for (int i = 0; i < program->nchildren; i++) {
        ASTNode *decl = program->children[i];
        if (decl->kind == ND_VAR_DECL)
            ir_add_global(prog, decl, decl->offset);
    }
}
