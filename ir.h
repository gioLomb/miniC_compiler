#ifndef IR_H
#define IR_H

#define IR_INITIAL_CAPACITY 64

#include "parser/ast.h"
#include "block.h"

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
        int tempId;
        struct {
            int varLevel;
            int varOffset;
            const char *sourceName;
        };
        int intVal;
        float floatVal;
        int labelId;
        const char *funcName;
    } data;
} Operand;

typedef struct {
    IROp    op;
    Operand dst, src1, src2;
    int     loopDepth;   /* annidamento loop statico, stampigliato da ir.c
                            durante irStmt(ND_WHILE). Sopravvive intatto
                            attraverso SVN/DCE/CP (copiano la struct intera).
                            LICM/SR lo aggiornano esplicitamente per le
                            istruzioni che spostano o inseriscono.
                            Usato da regalloc come peso (10^loopDepth) nel
                            costo di spill: variabile calda in loop interno
                            costa piu' spillarla di una fredda fuori. */
} IRInstr;

typedef struct {
    BasicBlock bb;         /* start, end, succ[2] condivisi con BasicBlock */
    int        predCount;  /* numero di predecessori nel CFG               */
} IRBlock;

typedef struct {
    char    *name;
    IRInstr *instrs;
    int      count, capacity;

    IRBlock *blocks;
    int      blockCount, blockCap;
    int      curBlockStart;

    int  labelBase;
    int *labelToBlock;
    int  labelToBlockCap;
} IRFunction;

typedef struct {
    IRFunction **functions;
    int          count, capacity;
} IRProgram;

IRProgram *ir_generate(ASTNode *program);
Operand    noOperand(void);
void       ir_print(const IRProgram *prog);
void       ir_free(IRProgram *prog);

#endif