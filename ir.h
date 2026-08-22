/**
 * @file ir.h
 * @brief Three-address code IR for the miniC compiler.
 */

#ifndef IR_H
#define IR_H

#include "parser/ast.h"
#include "block.h"
#include <stdlib.h>
#include "symbol_table.h"

#define IR_INITIAL_CAPACITY 64

/* =========================================================================
 * Instruction opcodes
 * ========================================================================= */

typedef enum {
    /* binary arithmetic / relational */
    IR_ADD, IR_SUB, IR_MUL, IR_DIV, IR_MOD,
    /* unary */
    IR_NEG, IR_NOT,
    /* comparisons (result 0 or 1) */
    IR_LT, IR_LE, IR_GT, IR_GE, IR_EQ, IR_NE,
    /* data movement */
    IR_ASSIGN,
    /**< dst = RIP-relative address of global identified by src1.data.globalOffset.
     *   Pure instruction (no side-effects, no memory write).
     *   src1 carries OPND_GLOBAL which is NOT a storage operand: liveness,
     *   VarMap, DCE all ignore it as a source. Emitted by ir_lower_globals()
     *   before any optimisation pass. */
    IR_GLOBAL_ADDR,
    IR_LOAD_ARR,    /**< dst = src1[src2]  */
    IR_STORE_ARR,   /**< dst[src1] = src2  */
    /* call convention */
    IR_PARAM, IR_CALL, IR_RETURN,
    /* control flow */
    IR_GOTO, IR_IF_FALSE, IR_LABEL,
} IROp;

/* =========================================================================
 * Operand kinds
 * ========================================================================= */

typedef enum {
    OPND_NONE,
    OPND_TEMP,          /**< compiler temporary                             */
    OPND_VAR,           /**< user variable, addressed by (varLevel,varOffset) */
    OPND_GLOBAL,        /**< global variable reference; carries only symOffset.
                         *   NOT a storage operand: never tracked by VarMap/liveness.
                         *   Survives only as src1 of IR_GLOBAL_ADDR after lowering. */
    OPND_CONST_INT,
    OPND_CONST_FLOAT,
    OPND_LABEL,
    OPND_FUNC
} OperandKind;

/* =========================================================================
 * Operand
 * ========================================================================= */

typedef struct {
    OperandKind kind;
    union {
        int tempId;
        struct {
            int varLevel;
            int varOffset;
            const char *sourceName;
        };
        int   intVal;
        float floatVal;
        int   labelId;
        const char *funcName;
        int   globalOffset;  /**< OPND_GLOBAL: symOffset matching IRGlobalVar.symOffset */
    } data;
} Operand;

/* =========================================================================
 * Instruction
 * ========================================================================= */

typedef struct {
    IROp    op;
    Operand dst, src1, src2;
    int     loopDepth;
} IRInstr;

/* =========================================================================
 * Basic block (IR level)
 * ========================================================================= */

typedef struct {
    BasicBlock bb;
    int        predCount;
} IRBlock;

/* =========================================================================
 * Function
 * ========================================================================= */

typedef struct {
    char    *name;
    IRInstr *instrs;
    int      count;
    int      capacity;

    IRBlock *blocks;
    int      blockCount;
    int      blockCap;
    int      curBlockStart;

    int  labelBase;
    int *labelToBlock;
    int  labelToBlockCap;

    /* Operandi (OPND_VAR) dei parametri formali, in ordine di dichiarazione.
     * Popolati da ir_buildFunction() cosi' che instr_selector.c possa
     * emettere, subito dopo il prologo, i MOV che legano i registri ABI
     * (rdi/rsi/...) ai vreg corrispondenti — altrimenti i parametri non
     * riceverebbero mai il loro valore all'ingresso della funzione. */
    Operand *params;
    int      paramCount;
} IRFunction;

/* =========================================================================
 * Global variable descriptor
 * ========================================================================= */

typedef struct {
    char    *name;
    DataType dataType;
    int      isArray;
    int      arraySize;
    int      symOffset;
    long    *initVals;
    int      initCount;
} IRGlobalVar;

/* =========================================================================
 * Program
 * ========================================================================= */

typedef struct {
    IRFunction **functions;
    int          count;
    int          capacity;

    IRGlobalVar *globals;
    int          globalCount;
    int          globalCap;
} IRProgram;

/* =========================================================================
 * Public API
 * ========================================================================= */

IRProgram *ir_generate(ASTNode *program);
int        ir_is_pure(IROp op);
int        ir_sweep(IRFunction *f, char *eliminate, int nBlocks);
Operand    noOperand(void);
void       ir_print(const IRProgram *prog);
void       ir_free(IRProgram *prog);

int ir_defines_dst(IROp op);
int ir_operand_is_storage(OperandKind kind);
int ir_isCommutative(IROp op);

#endif /* IR_H */