#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast.h"

#define INITIAL_CAPACITY 4

ASTNode *newNode(Arena *arena, NodeKind kind, const char *text) {
    ASTNode *node = malloc(sizeof(ASTNode));

    *node = (ASTNode){
        .kind       = kind,
        .text       = (arena && text) ? arena_strdup(arena, text) : NULL,
        .scopeLevel = -1,
        .offset     = -1,
    };

    return node;
}

void addChild(ASTNode *parent, ASTNode *child) {
    if (!parent || !child) return;
    if (parent->nchildren == parent->capacity) {
        parent->capacity = parent->capacity == 0 ? INITIAL_CAPACITY
                                                  : parent->capacity * 2;
        parent->children = realloc(parent->children,
                                   parent->capacity * sizeof(ASTNode *));
    }
    parent->children[parent->nchildren++] = child;
}

/* ---- tabella nomi (X-Macro, stesso ordine di NodeKind) ---- */
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
        printf("%s (%s)\n", kindName(node->kind), node->text);
    else
        printf("%s\n", kindName(node->kind));
    for (int i = 0; i < node->nchildren; i++)
        printAST(node->children[i], depth + 1);
}

void freeAST(ASTNode *node) {
    if (!node) return;
    for (int i = 0; i < node->nchildren; i++)
        freeAST(node->children[i]);
    free(node->children);
    /* node->text appartiene all'astArena: non va liberato qui */
    free(node);
}