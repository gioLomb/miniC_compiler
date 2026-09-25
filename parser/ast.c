#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../lexer.h"
#include "ast.h"

#define INITIAL_CAPACITY 4

ASTNode *newNode(Arena *arena, NodeKind kind, const char *text) {
    // Node struct now lives in the caller's arena: no per-node malloc/free
    // bookkeeping, the whole tree is reclaimed in bulk via arena_destroy().
    ASTNode *node = arena_alloc(arena, sizeof(ASTNode));

    *node = (ASTNode){
        .kind       = kind,
        .text       = text ? arena_strdup(arena, text) : NULL,
        .line       = lexer_current_line(),
        .scopeLevel = -1,
        .offset     = -1,
    };

    return node;
}

void addChild(Arena *arena, ASTNode *parent, ASTNode *child) {
    if (!parent || !child) return;

    if (parent->nchildren == parent->capacity) {
        int newCap = parent->capacity == 0 ? INITIAL_CAPACITY : parent->capacity * 2;

        ASTNode **grown = arena_alloc(arena, (size_t)newCap * sizeof(ASTNode *));
        if (parent->children)
            memcpy(grown, parent->children, (size_t)parent->nchildren * sizeof(ASTNode *));

        parent->children = grown;
        parent->capacity = newCap;
    }

    parent->children[parent->nchildren++] = child;
}

// Map NodeKind enum values to readable names using the X-Macro list
#define X_STR(val, str) str,
static const char *KIND_NAMES[] = {
    NODEKIND_LIST(X_STR)
};
#undef X_STR

static const char *kindName(NodeKind kind) {
    if ((unsigned)kind < sizeof(KIND_NAMES) / sizeof(KIND_NAMES[0]))
        return KIND_NAMES[kind];
    return "?";
}

void printAST(const ASTNode *node, int depth) {
    if (!node) return;

    for (int i = 0; i < depth; i++) printf("  ");

    if (node->text)
        printf("%s (%s) @L%d\n", kindName(node->kind), node->text, node->line);
    else
        printf("%s @L%d\n", kindName(node->kind), node->line);

    for (int i = 0; i < node->nchildren; i++){
        printAST(node->children[i], depth + 1);}
}


