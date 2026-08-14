#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast_to_symtab.h"

DataType symtab_type_from_string(const char *typeName) {
    if (!typeName) return T_VOID;

    // Check first character to determine data type
    switch (typeName[0]) {
    case 'i': return T_INT;    // Matches "int"
    case 'f': return T_FLOAT;  // Matches "float"
    default:  return T_VOID;
    }
}

void symtab_parse_decl_text(Arena *arena, const char *text,
                             char **outTypeName, char **outName,
                             int *isArray, int *arraySize) {
    // Reset output flags and parameters
    *isArray = 0;
    *arraySize = 0;
    *outTypeName = NULL;
    *outName = NULL;
    if (!text) return;

    // Duplicate string into arena to allow in-place modification without length bounds
    char *buf = arena_strdup(arena, text);

    // Locate whitespace separator between type and identifier name
    char *space = strchr(buf, ' ');
    if (!space) {
        *outTypeName = buf;
        return;
    }
    *space = '\0'; // Null-terminate type string in-place
    *outTypeName = buf;

    char *rest = space + 1;
    char *bracket = strchr(rest, '[');
    if (bracket) {
        // Handle array declaration format "name[size]"
        *bracket = '\0';
        *outName = rest;
        *isArray = 1;
        *arraySize = atoi(bracket + 1); // Parse integer size following bracket
    } else {
        // Standard variable or function identifier name
        *outName = rest;
    }
}

int symtab_declare_from_decl_text(Arena *arena, Scope *scope, ASTNode *node) {
    char *typeName, *name;
    int isArray, arraySize;

    // Parse declaration details from node text string
    symtab_parse_decl_text(arena, node->text, &typeName, &name, &isArray, &arraySize);

    // Build symbol table entry instance
    Symbol sym = {
        .kind       = SYM_VAR,
        .dataType   = symtab_type_from_string(typeName),
        .isArray    = isArray,
        .arraySize  = arraySize,
        .scopeLevel = scope->level,
        .offset     = (int)scope->table->size
    };

    // Register symbol into target scope and check for redeclaration conflicts
    if (!symtab_declare(scope, name, &sym)) {
        fprintf(stderr, "Errore: '%s' e' gia' stato dichiarato in questo scope\n", name);
        return 0;
    }

    // Attach lexical scope level and symbol offset coordinates to the AST node
    node->scopeLevel = sym.scopeLevel;
    node->offset     = sym.offset;
    return 1;
}

int symtab_populate_globals(ASTNode *program, Scope *global) {
    int errors = 0;

    // Allocate internal scratch arena for parsing top-level global declarations
    Arena *arena = arena_create(0);

    // Iterate over top-level statements/declarations of the program
    for (int i = 0; i < program->nchildren; i++) {
        ASTNode *decl = program->children[i];

        char *typeName, *name;
        int isArray, arraySize;
        symtab_parse_decl_text(arena, decl->text, &typeName, &name, &isArray, &arraySize);

        Symbol sym = {0};
        sym.dataType = symtab_type_from_string(typeName);

        // Process function declaration nodes
        if (decl->kind == ND_FUNC_DECL) {
            sym.kind = SYM_FUNC;

            // Determine parameter count (all children except the trailing function body block)
            int paramCount = decl->nchildren - 1;
            if (paramCount > SYM_MAX_PARAMS) {
                fprintf(stderr,
                        "Errore: '%s' ha %d parametri, il massimo supportato e' %d\n",
                        name, paramCount, SYM_MAX_PARAMS);
                paramCount = SYM_MAX_PARAMS;
                errors++;
            }
            sym.paramCount = paramCount;

            // Extract type signature for each parameter node
            for (int p = 0; p < paramCount; p++) {
                char *ptypeName, *pname;
                int pIsArray, pArraySize;
                symtab_parse_decl_text(arena, decl->children[p]->text,
                                        &ptypeName, &pname, &pIsArray, &pArraySize);

                DataType pType = symtab_type_from_string(ptypeName);
                symtab_pack_param_type(&sym.paramTypes, p, pType);
            }

        // Process global variable declaration nodes
        } else if (decl->kind == ND_VAR_DECL) {
            sym.kind = SYM_VAR;
            sym.isArray = isArray;
            sym.arraySize = arraySize;

        // Skip unexpected non-declaration nodes at global scope level
        } else {
            continue;
        }

        // Register global symbol and report redeclaration errors
        if (!symtab_declare(global, name, &sym)) {
            fprintf(stderr, "Errore: '%s' e' gia' stato dichiarato in questo scope\n", name);
            errors++;
        }
    }

    // Free scratch arena memory before returning
    arena_destroy(arena);
    return errors;
}