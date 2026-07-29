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
    IR_LABEL,
    //IR_NOP    /* istruzione morta, rimossa da dce_optimize() nella fase SWEEP */
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
            const char *sourceName;  /* non-owning */
        };
        int intVal;                 /* kind == OPND_CONST_INT */
        float floatVal;             /* kind == OPND_CONST_FLOAT */
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
    int start, end;
    int succ[2];
    int predCount;
} IRBlock;

typedef struct {
    char *name;
    IRInstr *instrs;
    int count, capacity;

    IRBlock *blocks;
    int blockCount, blockCap;
    int curBlockStart;

    int labelBase;
    int *labelToBlock;
    int labelToBlockCap;
} IRFunction;

typedef struct {
    IRFunction **functions;
    int count, capacity;
} IRProgram;

IRProgram *ir_generate(ASTNode *program);
Operand noOperand(void);
void ir_print(const IRProgram *prog);
void ir_free(IRProgram *prog);

#endif