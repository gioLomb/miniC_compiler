#ifndef IR_H
#define IR_H


/**
 * @file ir.h
 * @brief Three-address code IR for the miniC compiler.
 *
 * Defines the instruction opcodes, the linear IR data structures (functions,
 * basic blocks, instructions) and the builder / pretty-printer APIs.
 * Serves as the primary intermediate representation for all optimisation
 * and code-generation passes after the AST.
 */

#include "parser/ast.h"
#include "block.h"
#include <stdlib.h>
#include "symbol_table.h"
#include "parser/errorCollector.h"
//#include "varmap.h"

#define IR_INITIAL_CAPACITY 64  /**< Initial instrs[] capacity for a fresh IRFunction. */

// packs up to 2 operator characters into one 16-bit key for O(1) switch
// dispatch instead of strcmp chains
#define KEY_AND 0x2626
#define KEY_OR  0x7C7C
#define KEY_NOT 0x2100

/**
 * @brief Three-address IR opcode set.
 *
 * Values are used as bit indices into 32-bit predicate masks (ir_is_pure,
 * ir_defines_dst, ir_is_commutative), so the enum must stay within [0, 32)
 * and its relative ordering only matters where those masks are built.
 */
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
     *   VarMap, DCE all ignore it as a source. Emitted by gl_lower_globals()
     *   before any optimisation pass. */
    IR_GLOBAL_ADDR,
    IR_LOAD_ARR,    /**< dst = src1[src2]  */
    IR_STORE_ARR,   /**< dst[src1] = src2  */
    /* call convention */
    IR_PARAM, IR_CALL, IR_RETURN,
    /* control flow */
    IR_GOTO, IR_IF_FALSE, IR_LABEL,
} IROp;

/**
 * @brief Discriminant for the Operand union.
 */
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

/**
 * @brief Tagged union representing one IR value: variable, temp, constant,
 *        label, or function reference.
 */
typedef struct {
    OperandKind kind;
    int isFloat;   /**< 1 if this operand carries a float value; set at IR-
                     *   generation time from the AST's resolved type, never
                     *   inferred later. See instr_selector.c for consumers. */
    union {
        int tempId;                /**< OPND_TEMP: compiler-generated temporary id. */
        struct {
            int varLevel;          /**< OPND_VAR: lexical scope depth (0 reserved for globals pre-lowering). */
            int varOffset;         /**< OPND_VAR: slot offset within that scope. */
            const char *sourceName; /**< OPND_VAR: original source identifier, debug-print only. */
        };
        int   intVal;               /**< OPND_CONST_INT value. */
        float floatVal;             /**< OPND_CONST_FLOAT value. */
        int   labelId;               /**< OPND_LABEL: target label id. */
        const char *funcName;        /**< OPND_FUNC: callee name. */
        int   globalOffset;          /**< OPND_GLOBAL: symOffset matching IRGlobalVar.symOffset */
    } data;
} Operand;

/**
 * @brief One three-address IR instruction.
 */
typedef struct {
    IROp    op;
    Operand dst, src1, src2;
    int     loopDepth;  /**< Static loop nesting depth at the point this instruction was emitted. */
} IRInstr;

/**
 * @brief IR-level basic block: generic BasicBlock range plus predecessor count.
 */
typedef struct {
    BasicBlock bb;
    int        predCount;  /**< Number of CFG edges targeting this block. */
} IRBlock;

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

    Operand *params;
    int      paramCount;

    /**
     * @brief Structural-mutation counter for @c instrs[]/@c count.
     *
     * Bumped only when the instruction array is reindexed or rebuilt
     * (ir_sweep() removing entries, LICM hoisting, SR's loop rewrite,
     * global-variable lowering) — never for in-place operand edits (e.g.
     * CP folding a variable use to a constant). VarMap's per-instruction
     * id cache (see varmap_sync_cache()) uses this to know when cached
     * ids, keyed by instruction index, are no longer aligned with the
     * current array and must be recomputed.
     */
    int ver;
} IRFunction;

/**
 * @brief Descriptor for one global variable or array emitted to .data/.bss.
 */
typedef struct {
    char    *name;
    DataType dataType;
    int      isArray;
    int      arraySize;
    int      symOffset;   /**< Matches Operand.data.globalOffset for OPND_GLOBAL references. */
    long    *initVals;    /**< Per-element initializer values (int bits, or float bit-pattern); NULL if uninitialised. */
    int      initCount;   /**< Number of entries in initVals (0 = goes to .bss). */
} IRGlobalVar;

/**
 * @brief Top-level compiled program: all functions plus all global variables.
 */
typedef struct {
    IRFunction **functions;
    int          count;
    int          capacity;

    IRGlobalVar *globals;
    int          globalCount;
    int          globalCap;
} IRProgram;

/**
 * @brief Compressed-sparse-row predecessor list for a function's CFG.
 *
 * Built once in O(nBlocks) and shared by every pass that needs to walk a
 * block's incoming edges repeatedly (loop.c dominator/loop-body computation,
 * cp.c forward dataflow). Replaces the previous pattern of each pass
 * independently scanning every block's succ[] to find the predecessors of
 * a given block, which cost O(nBlocks^2) per fixed-point iteration.
 *
 * For block b, its predecessors are:
 *   predData[predStart[b] .. predStart[b] + predCount[b] - 1]
 *
 * @note This is a derived, read-only, single-direction (backward) view of
 *       succ[]. It must be rebuilt whenever succ[] changes (e.g. after CP's
 *       CFG pruning or loop_build_pre_header()'s edge rerouting) — it is
 *       not a substitute for succ[], only a cache of the reverse edges.
 */
typedef struct {
    int *predStart;  /**< predStart[b]: offset into predData for block b. */
    int *predCount;  /**< predCount[b]: number of predecessors of block b. */
    int *predData;   /**< Flat array of all predecessor block indices, CSR-packed. */
} PredList;

/**
 * @brief Translate a full AST program into an IRProgram.
 *
 * Runs, per function, the full optimisation pipeline (global lowering,
 * SVN, DCE, CP, LICM, SR) to a fixed point before returning.
 *
 * @param program Root ND_PROGRAM node produced by ParseProgram().
 * @return        Newly allocated IRProgram; release with ir_free().
 */
IRProgram *ir_generate(ASTNode *program);

/**
 * @brief Return non-zero if @p op is pure (no side effects, safe to reorder/eliminate).
 *
 * @param op IR opcode to test.
 * @return   1 if @p op only computes a value from its operands, 0 otherwise.
 */
int ir_is_pure(IROp op);

/**
 * @brief Tests two operands for structural identity (same kind and same storage location).
 *
 * @param a First operand.
 * @param b Second operand.
 * @return 1 if both operands reference the same variable or temporary, 0 otherwise.
 */
int ir_is_same_operand(const Operand *a, const Operand *b);

/**
 * @brief Compact @p f->instrs by removing every instruction flagged in @p eliminate.
 *
 * Rebuilds the instruction array in a single pass, rewriting each block's
 * [start, end) range to its new position. Used by every pass that marks
 * instructions dead (DCE, CP's CFG pruning) instead of physically deleting
 * them one at a time.
 *
 * @param f          IR function to compact (modified in place).
 * @param eliminate  Boolean array of length f->count; 1 marks an instruction for removal.
 * @param nBlocks    Number of blocks in f->blocks (== f->blockCount).
 * @return           1 if the instruction count changed, 0 if nothing was eliminated.
 */
int ir_sweep(IRFunction *f, char *eliminate, int nBlocks);

/**
 * @brief Print a human-readable dump of @p prog (globals + per-function instructions) to stdout.
 *
 * @param prog Program to print.
 */
void ir_print(const IRProgram *prog);

/**
 * @brief Free every resource owned by @p prog (functions, instructions, globals).
 *
 * @param prog Program to release; safe to call with NULL.
 */
void ir_free(IRProgram *prog);

/**
 * @brief Return non-zero if @p op writes a result into its dst operand.
 *
 * @param op IR opcode to test.
 * @return   1 if @p op defines a value in dst, 0 otherwise.
 */
int ir_defines_dst(IROp op);

/**
 * @brief Return non-zero if @p kind refers to a tracked storage location.
 *
 * Only OPND_VAR and OPND_TEMP are storage: they are the only kinds
 * assigned an id by VarMap and tracked by liveness. OPND_GLOBAL is
 * deliberately excluded — it survives only as the non-storage src1 of
 * IR_GLOBAL_ADDR after global lowering.
 *
 * @param kind Operand kind to test.
 * @return     1 if @p kind is OPND_VAR or OPND_TEMP, 0 otherwise.
 */
int ir_operand_is_storage(OperandKind kind);

/**
 * @brief Return non-zero if @p op is commutative (operand order irrelevant).
 *
 * Used by SVN to canonicalise expression keys so "a+b" and "b+a" hash to
 * the same value number.
 *
 * @param op IR opcode to test.
 * @return   1 for ADD, MUL, EQ, NE; 0 otherwise.
 */
int ir_is_commutative(IROp op);

/**
 * @brief Allocate and return a fresh, globally-unique temporary id.
 *
 * Draws from the same monotonic counter ir_generate() uses when emitting
 * OPND_TEMP operands during initial code generation, so ids handed out
 * here never collide with any temp already present in the program's IR —
 * no need to rescan the instruction stream to find a safe starting point.
 *
 * @return A tempId not used anywhere else in the current ir_generate() run.
 */
int ir_alloc_temp_id();

/**
 * @brief Build a CSR predecessor list for every block in @p f.
 *
 * Two-pass O(nBlocks) construction: first counts incoming edges per block
 * (one scan of every succ[] entry), then fills a flat array using running
 * offsets (prefix sum). No block-pair scan is ever performed, unlike the
 * O(nBlocks^2) pattern this replaces.
 *
 * @pre  @p f->blocks[].bb.succ[] must already be resolved (ir_resolve_cfg()
 *       has run, or the caller has otherwise populated succ[] manually,
 *       e.g. loop_build_pre_header()).
 *
 * @param f      IR function whose predecessor edges are derived from succ[].
 * @param arena  Arena for all PredList allocations.
 * @return       Populated PredList; valid as long as @p arena is alive.
 */
PredList ir_build_pred_list(IRFunction *f, Arena *arena);

#endif /* IR_H */