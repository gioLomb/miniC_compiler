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
 */

#ifndef IR_H
#define IR_H

#include "parser/ast.h"
#include "block.h"
#include <stdlib.h>
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
 *
 * Most instructions use the canonical form  dst = src1 op src2.
 * Exceptions:
 *   - IR_NEG / IR_NOT:  dst = op src1  (src2 unused)
 *   - IR_ASSIGN:        dst = src1     (src2 unused)
 *   - IR_STORE_ARR:     dst[src1] = src2
 *   - IR_PARAM:         src1 is the argument; dst/src2 unused
 *   - IR_CALL:          dst = call src1 (funcName); src2 = nArgs (CONST_INT)
 *   - IR_RETURN:        src1 is the return value
 *   - IR_GOTO:          dst is the target label
 *   - IR_IF_FALSE:      src1 is condition; dst is the target label
 *   - IR_LABEL:         dst carries the label id; src1/src2 unused
 *
 * loopDepth is stamped by ir_emitStmt when entering/leaving ND_WHILE nodes and
 * propagated intact through SVN/DCE/CP.  LICM and SR update it explicitly
 * for instructions they hoist or insert.  The register allocator uses it
 * to compute spill costs as 10^loopDepth, favouring keeping hot variables
 * in registers.
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
 *
 * BasicBlock (block.h) provides start, end, and succ[2].  IRBlock adds
 * predCount so that SVN can identify join points (blocks with more than
 * one predecessor) where value-numbering scope must be reset.
 */
typedef struct {
    BasicBlock bb;          /**< start/end indices into IRFunction.instrs; succ[2] */
    int        predCount;   /**< number of CFG predecessors                         */
} IRBlock;

/* =========================================================================
 * Function and program
 * ========================================================================= */

/**
 * @brief IR for a single function.
 *
 * instrs  — flat array of all instructions in textual (emission) order.
 * blocks  — array of IRBlock, each a [start,end) slice of instrs.
 *
 * labelBase / labelToBlock are temporary structures used during CFG
 * construction (emit → resolveCFG) and freed immediately after resolution.
 * curBlockStart tracks the start of the block currently being assembled.
 */
typedef struct {
    char    *name;          /**< function name (heap-allocated copy)         */
    IRInstr *instrs;        /**< instruction array (heap, grown via realloc) */
    int      count;         /**< number of valid instructions                */
    int      capacity;      /**< allocated capacity of instrs[]              */

    IRBlock *blocks;        /**< basic-block array (heap, grown via realloc) */
    int      blockCount;    /**< number of valid basic blocks                */
    int      blockCap;      /**< allocated capacity of blocks[]              */
    int      curBlockStart; /**< instruction index where the current block begins */

    int  labelBase;         /**< first label id allocated for this function  */
    int *labelToBlock;      /**< labelToBlock[id - labelBase] = block index  */
    int  labelToBlockCap;   /**< allocated capacity of labelToBlock[]        */
} IRFunction;

/**
 * @brief IR for an entire translation unit (list of IRFunction).
 */
typedef struct {
    IRFunction **functions; /**< heap array of function pointers             */
    int          count;     /**< number of valid functions                   */
    int          capacity;  /**< allocated capacity of functions[]           */
} IRProgram;

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * @brief Generate IR for an entire program and run all IR-level optimisations.
 *
 * Traverses every ND_FUNC_DECL in @p program, emits three-address
 * instructions, builds and resolves the CFG, then applies the optimisation
 * pipeline in order: SVN → DCE → CP/DCE loop → LICM+SR → CP/DCE loop.
 *
 * @param program Root AST node (ND_PROGRAM), fully semantic-checked.
 * @return        Heap-allocated IRProgram; caller must call ir_free().
 */
IRProgram *ir_generate(ASTNode *program);

/**
 * @brief Return non-zero if @p op is a pure, side-effect-free computation.
 *
 * Uses a bitmask for O(1) test.  Only pure instructions are candidates
 * for hoisting; impure instructions must remain in the loop body.
 *
 * @param op  IR opcode to test.
 * @return    1 if @p op is pure, 0 otherwise.
 */
int ir_is_pure(IROp op);


/**
 * @brief Return an operand of kind OPND_NONE (unused slot).
 */
Operand noOperand(void);

/**
 * @brief Print a human-readable dump of @p prog to stdout.
 *
 * Variables are shown as  vLEVEL.OFFSET[/name],  temporaries as  tN.
 * Intended for debugging; not used by the production pipeline.
 *
 * @param prog IR program to print.
 */
void ir_print(const IRProgram *prog);

/**
 * @brief Free all memory owned by @p prog, including all IRFunction objects.
 *
 * @param prog Program to destroy; may be NULL (no-op).
 */
void ir_free(IRProgram *prog);

/* =========================================================================
 * IR-front-end predicates
 * ========================================================================= */

/**
 * @brief Return non-zero if opcode @p op defines its destination operand.
 *
 * Uses a bitmask for O(1) lookup; opcodes that do not write a dst
 * (IR_STORE_ARR, IR_PARAM, IR_RETURN, IR_GOTO, IR_IF_FALSE, IR_LABEL)
 * are absent from the mask.
 */
int ir_defines_dst(IROp op);

/**
 * @brief Return non-zero if operand kind @p kind is a tracked variable or temp.
 *
 * Only OPND_VAR and OPND_TEMP contribute to the liveness sets; constants,
 * labels, and function names are transparent to the dataflow.
 */
int ir_operand_is_storage(OperandKind kind);

int ir_isCommutative(IROp op);
#endif /* IR_H */
