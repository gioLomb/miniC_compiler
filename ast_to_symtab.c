/**
 * @file ast_to_symtab.c
 * @brief AST → Symbol Table translation — implementation.
 *
 * Arena usage
 * -----------
 * st_elaborate_decl allocates all output substrings from the caller's arena
 * so they are reclaimed in bulk when the arena is destroyed.  Functions that
 * need only temporary substrings (st_bind_symbol, st_resolve_global_namespace)
 * create a local scratch arena and destroy it on return, avoiding lifetime
 * coupling between their temporaries and the AST arena.
 *
 * Offset assignment for globals
 * ------------------------------
 * st_resolve_global_namespace assigns sym.offset = global->table->size
 * before calling sym_bind, mirroring the sequential insertion order.
 * Both ND_VAR_DECL and ND_FUNC_DECL advance the implicit counter so that
 * ir_generate()'s symOffset loop produces the same sequence and OPND_VAR
 * operands correctly identify global variables.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast_to_symtab.h"

/* =========================================================================
 * Type resolution
 * ========================================================================= */

DataType st_resolve_type(const char *typeName) {
    if (!typeName) return T_VOID;

    // single-character dispatch: 'i'→int, 'f'→float, anything else→void
    switch (typeName[0]) {
    case 'i': return T_INT;
    case 'f': return T_FLOAT;
    default:  return T_VOID;
    }
}

/* =========================================================================
 * Declaration text parsing
 * ========================================================================= */

void st_elaborate_decl(Arena *arena, const char *text,
                       char **outTypeName, char **outName,
                       int *isArray, int *arraySize) {
    // initialise all outputs so partial failure leaves them in a known state
    *isArray   = 0;
    *arraySize = 0;
    *outTypeName = NULL;
    *outName     = NULL;
    if (!text) return;

    // work on a mutable arena copy so we can insert NUL terminators in-place
    char *buf = arena_strdup(arena, text);

    // split "TYPE NAME" or "TYPE NAME[SIZE]" on the first space
    char *space = strchr(buf, ' ');
    if (!space) {
        // malformed or type-only string; return type and leave name NULL
        *outTypeName = buf;
        return;
    }
    *space       = '\0';   // terminate type substring
    *outTypeName = buf;

    char *rest    = space + 1;
    char *bracket = strchr(rest, '[');
    if (bracket) {
        // array declaration: "NAME[SIZE]" — split on '[' and parse the size
        *bracket   = '\0';   // terminate name substring
        *outName   = rest;
        *isArray   = 1;
        *arraySize = atoi(bracket + 1);
    } else {
        // scalar declaration: name runs to end of string
        *outName = rest;
    }
}

/* =========================================================================
 * Single-symbol binding
 * ========================================================================= */

int st_bind_symbol(Arena *arena, Scope *scope, ASTNode *node) {
    char *typeName, *name;
    int   isArray, arraySize;

    // parse the compact declaration string stored in the AST node
    st_elaborate_decl(arena, node->text, &typeName, &name, &isArray, &arraySize);

    // build the Symbol descriptor; offset = next free slot in this scope
    Symbol sym = {
        .kind       = SYM_VAR,
        .dataType   = st_resolve_type(typeName),
        .isArray    = isArray,
        .arraySize  = arraySize,
        .scopeLevel = scope->level,
        .offset     = (int)scope->table->size,  // sequential within this scope
    };

    if (!sym_bind(scope, name, &sym)) {
        fprintf(stderr, "Errore: '%s' e' gia' stato dichiarato in questo scope\n", name);
        return 0;
    }

    // stamp the AST node so ir_generate() can resolve this variable without
    // a second symbol-table lookup
    node->scopeLevel = sym.scopeLevel;
    node->offset     = sym.offset;
    return 1;
}

static int st_process_func_decl(Arena *arena, ASTNode *decl, Symbol *sym, const char *name) {
    int errors = 0;
    sym->kind = SYM_FUNC;

    // last child is the body block; all preceding children are params
    int paramCount = decl->nchildren - 1;
    if (paramCount > SYM_MAX_PARAMS) {
        fprintf(stderr,
                "Errore: '%s' ha %d parametri, il massimo supportato e' %d\n",
                name, paramCount, SYM_MAX_PARAMS);
        paramCount = SYM_MAX_PARAMS;
        errors++;
    }
    sym->paramCount = paramCount;

    // pack each parameter's DataType into the 2-bit-per-param bitmask
    for (int p = 0; p < paramCount; p++) {
        char *ptypeName, *pname;
        int   pIsArray, pArraySize;
        st_elaborate_decl(arena, decl->children[p]->text,
                          &ptypeName, &pname, &pIsArray, &pArraySize);

        DataType pType = st_resolve_type(ptypeName);
        symtab_pack_param_type(&sym->paramTypes, p, pType);
    }

    return errors;
}

static inline void st_init_var_symbol(Symbol *sym, int isArray, int arraySize) {
    sym->kind      = SYM_VAR;
    sym->isArray   = isArray;
    sym->arraySize = arraySize;
    // dataType lasciato intatto: già settato dal chiamante prima dello switch
}

static inline int st_bind_global_symbol(Scope *global, const char *name, Symbol *sym) {
    // assign sequential offset BEFORE sym_bind so it equals the current
    // table size (number of entries already inserted).  Both vars and
    // funcs consume one slot so that ir_generate()'s symOffset counter
    // stays in sync with this ordering.
    sym->scopeLevel = global->level;             // always 0 for global scope
    sym->offset     = (int)global->table->size;  // next available slot

    if (!sym_bind(global, name, sym)) {
        fprintf(stderr, "Errore: '%s' e' gia' stato dichiarato in questo scope\n", name);
        return 1;
    }
    return 0;
}


int st_resolve_global_namespace(ASTNode *program, Scope *global) {
    int errors = 0;

    // scratch arena for temporary declaration substrings; destroyed on return
    Arena *arena = arena_create(0);

    for (int i = 0; i < program->nchildren; i++) {
        ASTNode *decl = program->children[i];

        char *typeName, *name;
        int   isArray, arraySize;
        st_elaborate_decl(arena, decl->text, &typeName, &name, &isArray, &arraySize);

        Symbol sym = {0};
        sym.dataType = st_resolve_type(typeName);

        switch (decl->kind) {
            case ND_FUNC_DECL:
                // ---- function declaration: collect parameter type signature ----
                errors += st_process_func_decl(arena, decl, &sym, name);
                break;

            case ND_VAR_DECL:
                // ---- global variable declaration ----
                st_init_var_symbol(&sym,isArray, arraySize);
                break;

            default:
                // ND_ERROR or any unexpected node kind: skip silently
                continue;
        }

        if (st_bind_global_symbol(global, name, &sym) == 0) {
            // stamp global ND_VAR_DECL nodes with their resolved coordinates
            // so ir_add_global() in ir.c can match them without a second lookup
            if (sym.kind == SYM_VAR) {
                decl->scopeLevel = sym.scopeLevel;
                decl->offset     = sym.offset;
            }
        } else {
            errors++;
        }
    }

    arena_destroy(arena);
    return errors;
}
