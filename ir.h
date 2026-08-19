/**
 * @file ir.h
 * @brief Three-address code IR (Intermediate Representation) for the miniC compiler.
 *
 * Defines the instruction set, operand kinds, and data structures for the
 * linear IR and its Control Flow Graph (CFG).  The IR sits between the
 * semantic-checked AST and the machine-code backend (instruction selector,
 * scheduler, register allocator).
 *
 * Pipeline position:
 *   AST → optimize_ast → ir_generate → [SVN → DCE → CP → LICM → SR] → isel
 *
 * Key design choices:
 *  - Instructions are stored in a flat array (IRFunction.instrs); basic blocks
 *    are index ranges [start, end) into that array, avoiding pointer chasing.
 *  - The CFG is built incrementally during code generation and resolved in a
 *    single post-pass (resolveCFG).
 *  - Every IRInstr carries a loopDepth field that survives all optimization
 *    passes and is consumed by the register allocator to weight spill costs.
 *  - Global variables are collected into IRProgram.globals so the instruction
 *    selector can emit .data/.bss sections and RIP-relative address loads.
 */

#ifndef IR_H
#define IR_H

#include "parser/ast.h"
#include "block.h"
#include <stdlib.h>
#include "symbol_table.h"
/** Initial capacity for the instruction array of a new IRFunction. */
#define IR_INITIAL_CAPACITY 64

/* =========================================================================
 * Instruction opcodes
 * ========================================================================= */

/**
 * @brief All IR opcodes in generation order.
 *
 * Arithmetic/logic: IR_ADD … IR_NE
 * Data movement:    IR_ASSIGN, IR_LOAD_ARR, IR_STORE_ARR
 * Call convention:  IR_PARAM, IR_CALL, IR_RETURN
 * Control flow:     IR_GOTO, IR_IF_FALSE, IR_LABEL
 */
typedef enum {
    /* --- binary arithmetic / relational --------------------------------- */
    IR_ADD,         /**< dst = src1 + src2                                  */
    IR_SUB,         /**< dst = src1 - src2                                  */
    IR_MUL,         /**< dst = src1 * src2                                  */
    IR_DIV,         /**< dst = src1 / src2                                  */
    IR_MOD,         /**< dst = src1 % src2                                  */
    /* --- unary ---------------------------------------------------------- */
    IR_NEG,         /**< dst = -src1                                        */
    IR_NOT,         /**< dst = !src1  (logical not, result 0 or 1)         */
    /* --- comparisons (result 0 or 1) ------------------------------------ */
    IR_LT,          /**< dst = src1 <  src2                                 */
    IR_LE,          /**< dst = src1 <= src2                                 */
    IR_GT,          /**< dst = src1 >  src2                                 */
    IR_GE,          /**< dst = src1 >= src2                                 */
    IR_EQ,          /**< dst = src1 == src2                                 */
    IR_NE,          /**< dst = src1 != src2                                 */
    /* --- data movement -------------------------------------------------- */
    IR_ASSIGN,      /**< dst = src1                                         */
    IR_LOAD_ARR,    /**< dst = src1[src2]  (array read)                     */
    IR_STORE_ARR,   /**< dst[src1] = src2  (array write)                    */
    /* --- call convention ------------------------------------------------ */
    IR_PARAM,       /**< push src1 as next call argument                    */
    IR_CALL,        /**< dst = call src1 (nArgs=src2)                       */
    IR_RETURN,      /**< return src1                                        */
    /* --- control flow --------------------------------------------------- */
    IR_GOTO,        /**< unconditional jump to dst (label)                  */
    IR_IF_FALSE,    /**< if (!src1) goto dst                                */
    IR_LABEL,       /**< branch target; dst carries the label id            */
} IROp;

/* =========================================================================
 * Operand kinds
 * ========================================================================= */

/**
 * @brief Discriminant for the Operand union.
 */
typedef enum {
    OPND_NONE,          /**< absent / not used                              */
    OPND_TEMP,          /**< compiler-generated temporary (SSA-like)        */
    OPND_VAR,           /**< user-declared variable, addressed by scope     */
    OPND_CONST_INT,     /**< integer constant                               */
    OPND_CONST_FLOAT,   /**< floating-point constant                        */
    OPND_LABEL,         /**< branch target label id                         */
    OPND_FUNC           /**< function name (for IR_CALL)                    */
} OperandKind;

/* =========================================================================
 * Operand
 * ========================================================================= */

/**
 * @brief A single IR operand — discriminated union over OperandKind.
 *
 * Variables are identified by (varLevel, varOffset) instead of by name so
 * that shadowed variables with the same name remain unambiguous.  The
 * optional sourceName pointer is kept for human-readable IR dumps only and
 * is never consulted by any analysis or transformation pass.
 *
 * Global variables have varLevel == 0 (global scope level). Their varOffset
 * matches the symOffset field of the corresponding IRGlobalVar descriptor,
 * which is used by the instruction selector to find the right global symbol.
 */
typedef struct {
    OperandKind kind;
    union {
        int tempId;         /**< OPND_TEMP: unique temp index                */
        struct {
            int varLevel;       /**< OPND_VAR: lexical scope depth           */
            int varOffset;      /**< OPND_VAR: slot index inside that scope  */
            const char *sourceName; /**< OPND_VAR: original identifier (debug only) */
        };
        int   intVal;       /**< OPND_CONST_INT                              */
        float floatVal;     /**< OPND_CONST_FLOAT                            */
        int   labelId;      /**< OPND_LABEL / IR_LABEL dst                   */
        const char *funcName; /**< OPND_FUNC: callee name string             */
    } data;
} Operand;

/* =========================================================================
 * Instruction
 * ========================================================================= */

/**
 * @brief A single three-address IR instruction.
 */
typedef struct {
    IROp    op;
    Operand dst, src1, src2;
    int     loopDepth;  /**< static loop-nesting depth at the point of emission */
} IRInstr;

/* =========================================================================
 * Basic block (IR level)
 * ========================================================================= */

/**
 * @brief IR-level basic block, extending the generic BasicBlock.
 */
typedef struct {
    BasicBlock bb;          /**< start/end indices into IRFunction.instrs; succ[2] */
    int        predCount;   /**< number of CFG predecessors                         */
} IRBlock;

/* =========================================================================
 * Function
 * ========================================================================= */

/**
 * @brief IR for a single function.
 */
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
} IRFunction;

/* =========================================================================
 * Global variable descriptor
 * =========================================================================
 * Populated by ir_generate() from global ND_VAR_DECL nodes.
 * Used by isel_emit_asm() to generate .data/.bss sections and by
 * select_function() to emit RIP-relative LEA/LOAD/STORE sequences.
 *
 * symOffset matches the `varOffset` field of OPND_VAR operands that
 * reference this global (both assigned sequentially by
 * st_resolve_global_namespace, which now sets sym.offset = table->size
 * before inserting each symbol).
 * ========================================================================= */

typedef struct {
    char    *name;       /**< Symbol name (heap-allocated copy).              */
    DataType dataType;   /**< T_INT or T_FLOAT.                               */
    int      isArray;    /**< 1 for arrays, 0 for scalars.                    */
    int      arraySize;  /**< Number of elements (arrays only; else 0).       */
    int      symOffset;  /**< Offset in the global symtab (== OPND_VAR.varOffset). */
    long    *initVals;   /**< Initializer values as long (IEEE-754 bits for floats);
                          *   NULL → goes to .bss (zero-init).               */
    int      initCount;  /**< Number of entries in initVals (0 → .bss).      */
} IRGlobalVar;

/* =========================================================================
 * Program
 * ========================================================================= */

/**
 * @brief IR for an entire translation unit (list of IRFunction + globals).
 */
typedef struct {
    IRFunction **functions; /**< heap array of function pointers             */
    int          count;     /**< number of valid functions                   */
    int          capacity;  /**< allocated capacity of functions[]           */

    IRGlobalVar *globals;      /**< global variable descriptors              */
    int          globalCount;  /**< number of valid global descriptors       */
    int          globalCap;    /**< allocated capacity of globals[]          */
} IRProgram;

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * @brief Generate IR for an entire program and run all IR-level optimisations.
 */
IRProgram *ir_generate(ASTNode *program);

/**
 * @brief Return non-zero if @p op is a pure, side-effect-free computation.
 */
int ir_is_pure(IROp op);

/**
 * @brief Compact the instruction array, removing eliminated instructions.
 */
int ir_sweep(IRFunction *f, char *eliminate, int nBlocks);

/**
 * @brief Return an operand of kind OPND_NONE (unused slot).
 */
Operand noOperand(void);

/**
 * @brief Print a human-readable dump of @p prog to stdout.
 */
void ir_print(const IRProgram *prog);

/**
 * @brief Free all memory owned by @p prog.
 */
void ir_free(IRProgram *prog);

/* =========================================================================
 * IR-front-end predicates
 * ========================================================================= */

int ir_defines_dst(IROp op);
int ir_operand_is_storage(OperandKind kind);
int ir_isCommutative(IROp op);

#endif /* IR_H */