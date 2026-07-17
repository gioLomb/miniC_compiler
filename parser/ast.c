#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast.h"

#define INITIAL_CAPACITY 4

ASTNode *newNode(NodeKind kind, const char *text) {
    ASTNode *node = (ASTNode *)malloc(sizeof(ASTNode));
    node->kind = kind;
    node->text = text ? strdup(text) : NULL;
    node->children = NULL;
    node->nchildren = 0;
    node->capacity = 0;
    node->scopeLevel = -1;
    node->offset = -1;
    return node;
}

void addChild(ASTNode *parent, ASTNode *child) {
    if (!parent || !child) return; /* consente addChild(node, NULL) senza effetti */
    if (parent->nchildren == parent->capacity) {
        parent->capacity = parent->capacity == 0 ? INITIAL_CAPACITY : parent->capacity * 2;
        parent->children = (ASTNode **)realloc(parent->children, parent->capacity * sizeof(ASTNode *));
    }
    parent->children[parent->nchildren++] = child;
}

static const char *kindName(NodeKind kind) {
    switch (kind) {
        case ND_PROGRAM:      return "Program";
        case ND_BLOCK:        return "Block";
        case ND_VAR_DECL:     return "VarDecl";
        case ND_FUNC_DECL:    return "FuncDecl";
        case ND_PARAM:        return "Param";
        case ND_IF:           return "If";
        case ND_WHILE:        return "While";
        case ND_RETURN:       return "Return";
        case ND_EXPR_STMT:    return "ExprStmt";
        case ND_ASSIGN:       return "Assign";
        case ND_BINOP:        return "BinOp";
        case ND_UNARY:        return "UnaryOp";
        case ND_CALL:         return "Call";
        case ND_ARRAY_ACCESS: return "ArrayAccess";
        case ND_ID:           return "Id";
        case ND_NUM_INT:      return "NumInt";
        case ND_NUM_FLOAT:    return "NumFloat";
        default:              return "?";
    }
}

void printAST(const ASTNode *node, int depth) {
    if (!node) return;
    for (int i = 0; i < depth; i++) printf("  ");
    if (node->text)
        printf("%s (%s)\n", kindName(node->kind), node->text);
    else
        printf("%s\n", kindName(node->kind));
    for (int i = 0; i < node->nchildren; i++) {
        printAST(node->children[i], depth + 1);
    }
}

void freeAST(ASTNode *node) {
    if (!node) return;
    for (int i = 0; i < node->nchildren; i++) {
        freeAST(node->children[i]);
    }
    free(node->children);
    free(node->text);
    free(node);
}