#ifndef IR_H
#define IR_H

#define IR_INITIAL_CAPACITY 64

#include "parser/ast.h"

typedef enum {
    IR_ADD, IR_SUB, IR_MUL, IR_DIV, IR_MOD,
    IR_NEG, IR_NOT,
    IR_LT, IR_LE, IR_GT, IR_GE, IR_EQ, IR_NE,
    IR_ASSIGN,
    IR_LOAD_ARR,
    IR_STORE_ARR,
    IR_PARAM,
    IR_CALL,
    IR_RETURN,
    IR_GOTO,
    IR_IF_FALSE,
    IR_LABEL
} IROp;

typedef enum {
    OPND_NONE,
    OPND_TEMP,
    OPND_VAR,
    OPND_CONST_INT,
    OPND_CONST_FLOAT,
    OPND_LABEL,
    OPND_FUNC
} OperandKind;

typedef struct {
    OperandKind kind;
    union {
        int tempId;                 /* kind == OPND_TEMP */
        struct {
            int varLevel;           /* kind == OPND_VAR */
            int varOffset;
            const char *sourceName;  /* non-owning: punta a node->text nell'AST */
        };
        long intVal;                /* kind == OPND_CONST_INT */
        double floatVal;            /* kind == OPND_CONST_FLOAT */
        int labelId;                /* kind == OPND_LABEL */
        const char *funcName;       /* kind == OPND_FUNC */
    } data;
} Operand;

typedef struct {
    IROp op;
    Operand dst, src1, src2;
} IRInstr;

/* ---- Blocco di base (CFG) ---- */
typedef struct {
    int start, end;      /* [start, end) in f->instrs */
    int succ[2];         /* indici di blocco, -1 se assente */
    int predCount;
} IRBlock;

typedef struct {
    char *name;
    IRInstr *instrs;
    int count, capacity;

    /* Blocchi costruiti live durante emit() */
    IRBlock *blocks;
    int blockCount, blockCap;
    int curBlockStart;          /* indice della prima istruzione del blocco corrente, -1 se nessuno */

    /* Mappa labelId -> indice di blocco (array denso) */
    int labelBase;              /* nextLabel all'inizio di questa funzione */
    int *labelToBlock;          /* indicizzato da labelId - labelBase */
    int labelToBlockCap;
} IRFunction;

typedef struct {
    IRFunction **functions;
    int count, capacity;
} IRProgram;

IRProgram *ir_generate(ASTNode *program);
void ir_print(const IRProgram *prog);
void ir_free(IRProgram *prog);

#endif