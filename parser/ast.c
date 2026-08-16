#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast.h"

#define INITIAL_CAPACITY 4

ASTNode *newNode(Arena *arena, NodeKind kind, const char *text) {
    // Allocate node container structure on the heap
    ASTNode *node = malloc(sizeof(ASTNode));

    // Duplicate text using memory arena if both are available
    *node = (ASTNode){
        .kind       = kind,
        .text       = (arena && text) ? arena_strdup(arena, text) : NULL,
        .scopeLevel = -1,
        .offset     = -1,
    };

    return node;
}

void addChild(ASTNode *parent, ASTNode *child) {
    // Guard clause against null pointers
    if (!parent || !child) return;

    // Expand children buffer capacity when array is full
    if (parent->nchildren == parent->capacity) {
        parent->capacity = parent->capacity == 0 ? INITIAL_CAPACITY
                                                  : parent->capacity * 2;
        parent->children = realloc(parent->children,
                                   parent->capacity * sizeof(ASTNode *));
    }

    // Append new child pointer and increment count
    parent->children[parent->nchildren++] = child;
}

// Map NodeKind enum values to readable names using the X-Macro list
#define X_STR(val, str) str,
static const char *KIND_NAMES[] = {
    NODEKIND_LIST(X_STR)
};
#undef X_STR

/**
 * @brief Internal helper to safely retrieve string representation for a NodeKind.
 */
static const char *kindName(NodeKind kind) {
    // Return matching string if within array bounds; fallback to placeholder
    if ((unsigned)kind < sizeof(KIND_NAMES) / sizeof(KIND_NAMES[0]))
        return KIND_NAMES[kind];
    return "?";
}

void printAST(const ASTNode *node, int depth) {
    if (!node) return;

    // Apply tree level indentation spaces
    for (int i = 0; i < depth; i++) printf("  ");

    // Print node type name along with text payload if present
    if (node->text)
        printf("%s (%s)\n", kindName(node->kind), node->text);
    else
        printf("%s\n", kindName(node->kind));

    // Recursively print all child branches with incremented depth
    for (int i = 0; i < node->nchildren; i++)
        printAST(node->children[i], depth + 1);
}

void freeAST(ASTNode *node) {
    if (!node) return;

    // Recursively free sub-tree children first
    for (int i = 0; i < node->nchildren; i++)
        freeAST(node->children[i]);

    // Free dynamic children pointer array
    free(node->children);

    // Note: node->text is owned by arena allocator and must not be freed here
    free(node);
}
