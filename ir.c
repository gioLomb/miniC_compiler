/**
 * @file ir.c
 * @brief IR generation from the AST and IR-level optimisation pipeline.
 *
 * Responsibilities of this module:
 *  1. Translate every ND_FUNC_DECL in the AST into a flat IRInstr array
 *     (irStmt / irExpr / irExprInto).
 *  2. Build the Control Flow Graph (CFG) incrementally during emission
 *     and resolve label→block mappings in a single post-pass (resolveCFG).
 *  3. Run the IR-level optimisation pipeline on each function:
 *       SVN → DCE → (CP + DCE)* → LICM + SR → (CP + DCE)*
 *  4. Expose ir_print and ir_free for debugging and cleanup.
 *
 * Code-generation strategy
 * ------------------------
 * Two mutually recursive families of functions handle expressions:
 *
 *   irExpr(expr, f)           — evaluates expr, emits into a fresh temp,
 *                               returns the result Operand.
 *   irExprInto(expr, f, dest) — same but writes directly into dest
 *                               (a named variable or existing temp).
 *                               Avoids one IR_ASSIGN copy for the common
 *                               "var = expr" pattern.
 *
 * Short-circuit Boolean operators (&& / ||) are handled by
 * irJumpIfFalse / irJumpIfTrue, which emit conditional branches directly
 * without materialising a Boolean into a temporary first.  When a Boolean
 * value *must* be materialised (e.g. "r = a && b"), irShortCircuit[Into]
 * wraps the jump sequence with two IR_ASSIGN(0/1) arms.
 *
 * CFG construction
 * ----------------
 * emit() closes the current basic block and opens a new one whenever it
 * encounters an IR_LABEL (new block starts) or a terminator opcode (GOTO /
 * IF_FALSE / RETURN — the current block ends).  After all instructions of
 * a function have been emitted, resolveCFG() walks the block list, reads
 * each block's terminator to determine successor block indices, and fills
 * IRBlock.bb.succ[].  The temporary labelToBlock array is freed there.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ir.h"
#include "svn.h"
#include "dce.h"
#include "cp.h"
#include "licm.h"
#include "sr.h"
#include "sched.h"
#include "arena.h"

/* =========================================================================
 * Operator key helpers
 * -------------------------------------------------------------------------
 * Pack two chars into a 16-bit key to avoid strcmp in hot paths.
 * op_key("==") → 0x3D3D, op_key("+") → 0x2B00, etc.
 * ========================================================================= */
#define KEY_AND 0x2626  /* "&&" */
#define KEY_OR  0x7C7C  /* "||" */
#define KEY_NOT 0x2100  /* "!"  */

/** Pack the first two chars of @p s into a 16-bit key. */
static inline unsigned short op_key(const char *s) {
    if (!s || !s[0]) return 0;
    unsigned short k = (unsigned char)s[0] << 8;
    if (s[1]) k |= (unsigned char)s[1];
    return k;
}

/* =========================================================================
 * Module-level mutable state
 * -------------------------------------------------------------------------
 * These are reset at the start of each ir_generate() call and updated
 * throughout the generation of a single translation unit.
 * ========================================================================= */

static int nextTemp;            /**< counter for fresh temporary ids             */
static int nextLabel;           /**< counter for fresh label ids (global scope)  */
static int currentLoopDepth;    /**< nesting depth of the innermost while loop   */

/* =========================================================================
 * Operand constructors
 * -------------------------------------------------------------------------
 * Each constructor zero-initialises the union via a compound literal,
 * ensuring no stale bits leak between uses.
 * ========================================================================= */

/** Allocate a fresh, unique temporary operand. */
static inline Operand mkTemp(void) {
    return (Operand){ .kind = OPND_TEMP, .data.tempId = nextTemp++ };
}

/** Allocate a fresh, unique label operand. */
static inline Operand mkLabel(void) {
    return (Operand){ .kind = OPND_LABEL, .data.labelId = nextLabel++ };
}

/**
 * Build a variable operand from a resolved AST node.
 * scopeLevel and offset were stamped by semantic_check(); sourceName is
 * kept for debug dumps only.
 */
static inline Operand mkVar(const ASTNode *node) {
    return (Operand){ .kind           = OPND_VAR,
                      .data.varLevel  = node->scopeLevel,
                      .data.varOffset = node->offset,
                      .data.sourceName = node->text };
}

/** Build an integer constant operand. */
static inline Operand mkConstInt(int v) {
    return (Operand){ .kind = OPND_CONST_INT, .data.intVal = v };
}

/** Build a float constant operand. */
static inline Operand mkConstFloat(float v) {
    return (Operand){ .kind = OPND_CONST_FLOAT, .data.floatVal = v };
}

/** Build a function-name operand (for IR_CALL src1). */
static inline Operand mkFunc(const char *name) {
    return (Operand){ .kind = OPND_FUNC, .data.funcName = name };
}

/** Return an inert "no operand" sentinel. */
Operand noOperand(void) {
    return (Operand){ .kind = OPND_NONE };
}

/* =========================================================================
 * CFG helpers
 * ========================================================================= */

/**
 * @brief Return non-zero if @p op is a basic-block terminator.
 *
 * Terminators (GOTO, IF_FALSE, RETURN) end the current basic block;
 * the next instruction opens a new one.  Uses a bitmask for O(1) test.
 */
static inline int isTerminator(IROp op) {
    const unsigned int mask =
        (1U << IR_GOTO) | (1U << IR_IF_FALSE) | (1U << IR_RETURN);
    return (mask & (1U << op)) != 0;
}

/**
 * @brief Append a new IRBlock covering instrs[start..end) to @p f.
 *
 * Grows f->blocks via realloc if necessary.  The new block is initialised
 * with succ[0]=succ[1]=-1 and predCount=0; resolveCFG() fills succ[] later.
 *
 * @param f     Target function.
 * @param start First instruction index (inclusive).
 * @param end   One-past-last instruction index.
 */
static inline void closeBlock(IRFunction *f, int start, int end) {
    if (end <= start) return;     // skip degenerate empty ranges
    if (f->blockCount == f->blockCap) {
        f->blockCap = f->blockCap ? f->blockCap * 2 : 16;
        f->blocks = realloc(f->blocks, (size_t)f->blockCap * sizeof(IRBlock));
    }
    f->blocks[f->blockCount++] = (IRBlock){
        .bb       = { .start = start, .end = end, .succ = {-1, -1} },
        .predCount = 0,
    };
}

/**
 * @brief Record that label @p labelId targets block index @p futureBlockIdx.
 *
 * The labelToBlock array is indexed by (labelId - f->labelBase) and is
 * grown on demand.  It is freed by resolveCFG().
 *
 * @param f              Target function.
 * @param labelId        Globally unique label id.
 * @param futureBlockIdx Block index that the label will head.
 */
static void registerLabel(IRFunction *f, int labelId, int futureBlockIdx) {
    int idx = labelId - f->labelBase;
    if (idx >= f->labelToBlockCap) {
        // Double capacity until it fits, initialise new slots to -1.
        int newCap = f->labelToBlockCap ? f->labelToBlockCap : 8;
        while (newCap <= idx) newCap <<= 1;
        int *newTable = realloc(f->labelToBlock, (size_t)newCap * sizeof(int));
        if (!newTable) { fprintf(stderr, "OOM in registerLabel\n"); exit(1); }
        f->labelToBlock = newTable;
        memset(f->labelToBlock + f->labelToBlockCap, -1,
               (size_t)(newCap - f->labelToBlockCap) * sizeof(int));
        f->labelToBlockCap = newCap;
    }
    f->labelToBlock[idx] = futureBlockIdx;
}

/**
 * @brief Emit one IR instruction into @p f, maintaining CFG block boundaries.
 *
 * Rules:
 *  - An IR_LABEL at a non-empty position closes the current block first,
 *    then registers the label as the head of the next block.
 *  - Any terminator (GOTO / IF_FALSE / RETURN) closes the current block
 *    after appending the instruction.
 *
 * loopDepth is stamped from the module-level currentLoopDepth at emit time.
 *
 * @param f    Target function.
 * @param op   Opcode.
 * @param dst  Destination operand (noOperand() if unused).
 * @param src1 First source operand.
 * @param src2 Second source operand.
 */
static void emit(IRFunction *f, IROp op, Operand dst, Operand src1, Operand src2) {
    // Grow instruction array if full.
    if (f->count == f->capacity) {
        f->capacity = f->capacity ? f->capacity * 2 : 16;
        f->instrs = realloc(f->instrs, (size_t)f->capacity * sizeof(IRInstr));
    }
    int idx = f->count;

    // A label in the middle of a block ends the current block.
    if (op == IR_LABEL && idx > f->curBlockStart) {
        closeBlock(f, f->curBlockStart, idx);
        f->curBlockStart = idx;
    }

    f->instrs[idx] = (IRInstr){
        .op        = op,
        .dst       = dst,
        .src1      = src1,
        .src2      = src2,
        .loopDepth = currentLoopDepth,
    };
    f->count++;

    if (op == IR_LABEL)
        registerLabel(f, dst.data.labelId, f->blockCount);

    // Terminators end the current block; next instruction starts a new one.
    if (isTerminator(op)) {
        closeBlock(f, f->curBlockStart, f->count);
        f->curBlockStart = f->count;
    }
}

/* Convenience wrappers to keep call sites readable. */
static inline void emitGoto(IRFunction *f, Operand label) {
    emit(f, IR_GOTO, label, noOperand(), noOperand());
}
static inline void emitIfFalse(IRFunction *f, Operand cond, Operand label) {
    emit(f, IR_IF_FALSE, label, cond, noOperand());
}
static inline void emitLabel(IRFunction *f, Operand label) {
    emit(f, IR_LABEL, label, noOperand(), noOperand());
}

/* =========================================================================
 * CFG resolution
 * ========================================================================= */

/**
 * @brief Finalise the CFG for @p f after all instructions have been emitted.
 *
 * Steps:
 *  1. Close any trailing open block.
 *  2. For each block, examine its last instruction to set succ[]:
 *       - IR_GOTO        → succ[0] = target block
 *       - IR_IF_FALSE    → succ[0] = fall-through, succ[1] = taken target
 *       - other / RETURN → succ[0] = next block (or -1)
 *  3. Increment predCount of each referenced successor.
 *  4. Free the temporary labelToBlock array.
 */
static void resolveCFG(IRFunction *f) {
    // Close any block that was still open at the end of the function.
    if (f->curBlockStart < f->count) {
        closeBlock(f, f->curBlockStart, f->count);
        f->curBlockStart = f->count;
    }

    for (int b = 0; b < f->blockCount; b++) {
        int last = f->blocks[b].bb.end - 1;
        IROp op  = f->instrs[last].op;

        if (op == IR_GOTO) {
            int lbl = f->instrs[last].dst.data.labelId - f->labelBase;
            f->blocks[b].bb.succ[0] =
                (lbl >= 0 && lbl < f->labelToBlockCap) ? f->labelToBlock[lbl] : -1;

        } else if (op == IR_IF_FALSE) {
            // succ[0] = fall-through (condition true), succ[1] = branch taken
            f->blocks[b].bb.succ[0] = (b + 1 < f->blockCount) ? b + 1 : -1;
            int lbl = f->instrs[last].dst.data.labelId - f->labelBase;
            f->blocks[b].bb.succ[1] =
                (lbl >= 0 && lbl < f->labelToBlockCap) ? f->labelToBlock[lbl] : -1;

        } else if (op != IR_RETURN) {
            // Non-terminator last instruction: fall through to next block.
            f->blocks[b].bb.succ[0] = (b + 1 < f->blockCount) ? b + 1 : -1;
        }
    }

    // Increment predCount for every successor.
    for (int b = 0; b < f->blockCount; b++)
        for (int k = 0; k < 2; k++) {
            int s = f->blocks[b].bb.succ[k];
            if (s >= 0) f->blocks[s].predCount++;
        }

    // labelToBlock is only needed during construction; free it now.
    free(f->labelToBlock);
    f->labelToBlock    = NULL;
    f->labelToBlockCap = 0;
}

/* =========================================================================
 * Expression code generation — forward declarations
 * ========================================================================= */

static Operand irExpr(ASTNode *expr, IRFunction *out);
static Operand irExprInto(ASTNode *expr, IRFunction *out, Operand dest);
static void    irJumpIfFalse(ASTNode *cond, IRFunction *out, Operand falseLbl);
static void    irJumpIfTrue (ASTNode *cond, IRFunction *out, Operand trueLbl);

/* =========================================================================
 * Binary operator → IROp mapping
 * ========================================================================= */

/**
 * @brief Map a two-character operator string to the corresponding IROp.
 *
 * Uses a precomputed 16-bit key (high byte = first char, low byte = second
 * char or 0) to avoid strcmp.  Unmapped operators default to IR_ADD as a
 * safe fallback (should never happen after semantic analysis).
 *
 * @param op Operator string as produced by the parser (e.g. "+", "<=").
 * @return   Corresponding IROp value.
 */
static inline IROp binopToIROp(const char *op) {
    if (!op || op[0] == '\0') return IR_ADD;
    unsigned short key =
        (unsigned short)(((unsigned char)op[0] << 8) |
                         (unsigned char)(op[1] ? op[1] : 0));
    switch (key) {
    case (('+')<<8)|0:   return IR_ADD;
    case (('-')<<8)|0:   return IR_SUB;
    case (('*')<<8)|0:   return IR_MUL;
    case (('/')<<8)|0:   return IR_DIV;
    case (('%')<<8)|0:   return IR_MOD;
    case (('<')<<8)|0:   return IR_LT;
    case (('>')<<8)|0:   return IR_GT;
    case (('<')<<8)|'=': return IR_LE;
    case (('>')<<8)|'=': return IR_GE;
    case (('=')<<8)|'=': return IR_EQ;
    case (('!')<<8)|'=': return IR_NE;
    default:             return IR_ADD;
    }
}

/* =========================================================================
 * Short-circuit Boolean code generation
 * -------------------------------------------------------------------------
 * irJumpIfFalse / irJumpIfTrue implement short-circuit evaluation without
 * materialising a Boolean temporary.  They recurse on the AST to emit a
 * minimal sequence of conditional branches.
 *
 *   &&:  irJumpIfFalse(a && b, L) ≡  if(!a) goto L; if(!b) goto L;
 *   ||:  irJumpIfFalse(a || b, L) ≡  if(a) goto skip; if(!b) goto L; skip:
 *   !:   irJumpIfFalse(!e, L)     ≡  irJumpIfTrue(e, L)
 *   other: evaluate e, emit IF_FALSE e goto L
 * ========================================================================= */

/**
 * @brief Emit code that jumps to @p falseLbl if @p cond evaluates to false.
 *
 * Handles &&, ||, and ! specially to produce short-circuit code.
 * For all other expression kinds, falls back to irExpr + IF_FALSE.
 */
static void irJumpIfFalse(ASTNode *cond, IRFunction *out, Operand falseLbl) {
    if (cond->kind == ND_BINOP && op_key(cond->text) == KEY_AND) {
        // a && b: both must be true — jump on first failure
        irJumpIfFalse(cond->children[0], out, falseLbl);
        irJumpIfFalse(cond->children[1], out, falseLbl);
        return;
    }
    if (cond->kind == ND_BINOP && op_key(cond->text) == KEY_OR) {
        // a || b: jump only if *both* fail
        Operand skipLbl = mkLabel();
        irJumpIfTrue(cond->children[0], out, skipLbl);   // left true → skip
        irJumpIfFalse(cond->children[1], out, falseLbl); // right false → jump
        emitLabel(out, skipLbl);
        return;
    }
    if (cond->kind == ND_UNARY && op_key(cond->text) == KEY_NOT) {
        // !e is false ↔ e is true
        irJumpIfTrue(cond->children[0], out, falseLbl);
        return;
    }
    // General case: evaluate and branch.
    Operand v = irExpr(cond, out);
    emitIfFalse(out, v, falseLbl);
}

/**
 * @brief Emit code that jumps to @p trueLbl if @p cond evaluates to true.
 *
 * Dual of irJumpIfFalse; used to implement || in irJumpIfFalse and
 * to build irShortCircuit.
 */
static void irJumpIfTrue(ASTNode *cond, IRFunction *out, Operand trueLbl) {
    if (cond->kind == ND_BINOP && op_key(cond->text) == KEY_AND) {
        // a && b is true ↔ a is true *and* b is true
        Operand skipLbl = mkLabel();
        irJumpIfFalse(cond->children[0], out, skipLbl);
        irJumpIfTrue(cond->children[1], out, trueLbl);
        emitLabel(out, skipLbl);
        return;
    }
    if (cond->kind == ND_BINOP && op_key(cond->text) == KEY_OR) {
        // a || b is true if either operand is true
        irJumpIfTrue(cond->children[0], out, trueLbl);
        irJumpIfTrue(cond->children[1], out, trueLbl);
        return;
    }
    if (cond->kind == ND_UNARY && op_key(cond->text) == KEY_NOT) {
        irJumpIfFalse(cond->children[0], out, trueLbl);
        return;
    }
    // General case: evaluate, jump over if false, unconditional jump if true.
    Operand v       = irExpr(cond, out);
    Operand skipLbl = mkLabel();
    emitIfFalse(out, v, skipLbl);
    emitGoto(out, trueLbl);
    emitLabel(out, skipLbl);
}

/**
 * @brief Materialise the short-circuit Boolean @p expr into @p dest.
 *
 * Emits the pattern:
 *   if (!expr) goto false_lbl
 *   dest = 1; goto end_lbl
 *   false_lbl: dest = 0
 *   end_lbl:
 *
 * @param expr Short-circuit expression (&& or ||) to evaluate.
 * @param out  Target function.
 * @param dest Destination operand to receive 0 or 1.
 * @return     @p dest.
 */
static Operand irShortCircuitInto(ASTNode *expr, IRFunction *out, Operand dest) {
    Operand endLbl   = mkLabel();
    Operand falseLbl = mkLabel();
    irJumpIfFalse(expr, out, falseLbl);
    emit(out, IR_ASSIGN, dest, mkConstInt(1), noOperand());
    emitGoto(out, endLbl);
    emitLabel(out, falseLbl);
    emit(out, IR_ASSIGN, dest, mkConstInt(0), noOperand());
    emitLabel(out, endLbl);
    return dest;
}

/** Variant of irShortCircuitInto that allocates a fresh temp as destination. */
static Operand irShortCircuit(ASTNode *expr, IRFunction *out) {
    return irShortCircuitInto(expr, out, mkTemp());
}

/* =========================================================================
 * Assignment and call helpers
 * ========================================================================= */

/**
 * @brief Emit code for an assignment expression (ND_ASSIGN).
 *
 * Scalar:  lvalue = rvalue  →  irExprInto(rvalue, mkVar(lvalue))
 * Array:   lvalue[idx] = rvalue  →  IR_STORE_ARR dst, idx, rvalue
 *
 * @return Operand holding the assigned value.
 */
static Operand irAssign(ASTNode *expr, IRFunction *out) {
    ASTNode *lvalue = expr->children[0];
    if (lvalue->kind == ND_ID)
        // Scalar: write directly into the named variable, avoids a copy.
        return irExprInto(expr->children[1], out, mkVar(lvalue));

    // Array element: evaluate index and RHS, emit a store.
    Operand idx  = irExpr(lvalue->children[0], out);
    Operand base = mkVar(lvalue);
    Operand rhs  = irExpr(expr->children[1], out);
    emit(out, IR_STORE_ARR, base, idx, rhs);
    return rhs;
}

/**
 * @brief Emit code for a function call expression (ND_CALL).
 *
 * Pushes arguments via IR_PARAM in left-to-right order, then emits IR_CALL.
 * The number of arguments is encoded in IR_CALL.src2 as a CONST_INT so that
 * the instruction selector can determine stack cleanup without re-traversing
 * the IR.
 *
 * @return Fresh temporary holding the call result.
 */
static Operand irCall(ASTNode *expr, IRFunction *out) {
    // Emit one IR_PARAM per argument in declaration order.
    for (int i = 0; i < expr->nchildren; i++) {
        Operand arg = irExpr(expr->children[i], out);
        emit(out, IR_PARAM, noOperand(), arg, noOperand());
    }
    Operand result = mkTemp();
    emit(out, IR_CALL, result, mkFunc(expr->text), mkConstInt(expr->nchildren));
    return result;
}

/* =========================================================================
 * irExpr — general expression → fresh temp
 * ========================================================================= */

/**
 * @brief Emit code for @p expr, returning the Operand that holds the value.
 *
 * For simple operands (constants, variables) the Operand is returned
 * directly without emitting any instruction.  For compound expressions a
 * fresh temporary is allocated and the result is stored there.
 *
 * @param expr AST expression node.
 * @param out  Target function.
 * @return     Operand containing the expression result.
 */
static Operand irExpr(ASTNode *expr, IRFunction *out) {
    switch (expr->kind) {
    case ND_NUM_INT:   return mkConstInt(atoi(expr->text));
    case ND_NUM_FLOAT: return mkConstFloat((float)atof(expr->text));
    case ND_ID:        return mkVar(expr);

    case ND_ARRAY_ACCESS: {
        Operand idx  = irExpr(expr->children[0], out);
        Operand base = mkVar(expr);
        Operand t    = mkTemp();
        emit(out, IR_LOAD_ARR, t, base, idx);
        return t;
    }

    case ND_UNARY: {
        Operand v = irExpr(expr->children[0], out);
        Operand t = mkTemp();
        emit(out, op_key(expr->text) == KEY_NOT ? IR_NOT : IR_NEG, t, v, noOperand());
        return t;
    }

    case ND_BINOP: {
        unsigned short key = op_key(expr->text);
        // && and || require short-circuit evaluation.
        if (key == KEY_AND || key == KEY_OR)
            return irShortCircuit(expr, out);
        Operand lhs = irExpr(expr->children[0], out);
        Operand rhs = irExpr(expr->children[1], out);
        Operand t   = mkTemp();
        emit(out, binopToIROp(expr->text), t, lhs, rhs);
        return t;
    }

    case ND_ASSIGN: return irAssign(expr, out);
    case ND_CALL:   return irCall(expr, out);
    default:        return noOperand();
    }
}

/**
 * @brief Emit code for @p expr, placing the result directly into @p dest.
 *
 * Falls back to irExpr + IR_ASSIGN for expression kinds where direct
 * emission into an arbitrary destination is not straightforward.
 *
 * @param expr AST expression node.
 * @param out  Target function.
 * @param dest Pre-allocated destination operand (VAR or TEMP).
 * @return     @p dest.
 */
static Operand irExprInto(ASTNode *expr, IRFunction *out, Operand dest) {
    switch (expr->kind) {
    case ND_NUM_INT:
        emit(out, IR_ASSIGN, dest, mkConstInt(atoi(expr->text)), noOperand());
        return dest;
    case ND_NUM_FLOAT:
        emit(out, IR_ASSIGN, dest, mkConstFloat((float)atof(expr->text)), noOperand());
        return dest;
    case ND_ID:
        emit(out, IR_ASSIGN, dest, mkVar(expr), noOperand());
        return dest;

    case ND_ARRAY_ACCESS: {
        Operand idx  = irExpr(expr->children[0], out);
        Operand base = mkVar(expr);
        emit(out, IR_LOAD_ARR, dest, base, idx);
        return dest;
    }

    case ND_UNARY: {
        Operand v = irExpr(expr->children[0], out);
        emit(out, op_key(expr->text) == KEY_NOT ? IR_NOT : IR_NEG, dest, v, noOperand());
        return dest;
    }

    case ND_BINOP: {
        unsigned short key = op_key(expr->text);
        if (key == KEY_AND || key == KEY_OR)
            return irShortCircuitInto(expr, out, dest);
        Operand lhs = irExpr(expr->children[0], out);
        Operand rhs = irExpr(expr->children[1], out);
        emit(out, binopToIROp(expr->text), dest, lhs, rhs);
        return dest;
    }

    case ND_CALL: {
        for (int i = 0; i < expr->nchildren; i++) {
            Operand arg = irExpr(expr->children[i], out);
            emit(out, IR_PARAM, noOperand(), arg, noOperand());
        }
        emit(out, IR_CALL, dest, mkFunc(expr->text), mkConstInt(expr->nchildren));
        return dest;
    }

    case ND_ASSIGN: {
        // Evaluate the nested assignment first, then copy its result to dest.
        Operand inner = irAssign(expr, out);
        emit(out, IR_ASSIGN, dest, inner, noOperand());
        return dest;
    }

    default: return noOperand();
    }
}

/* =========================================================================
 * Statement code generation
 * ========================================================================= */

/**
 * @brief Recursively emit IR for statement @p stmt into function @p out.
 *
 * Statement kinds and their translation:
 *
 *  ND_BLOCK      — emit children in order.
 *  ND_VAR_DECL   — emit initialiser(s) if present; array initialisers
 *                  become a sequence of IR_STORE_ARR.
 *  ND_EXPR_STMT  — emit the child expression (result discarded).
 *  ND_IF         — emit condition with irJumpIfFalse; emit then-branch;
 *                  if else present, bracket with GOTO + LABEL.
 *  ND_WHILE      — emit start-label, condition test, body (with incremented
 *                  currentLoopDepth), back-edge GOTO, end-label.
 *  ND_RETURN     — evaluate return expression, emit IR_RETURN.
 *
 * @param stmt AST statement node.
 * @param out  Target function.
 */
static void irStmt(ASTNode *stmt, IRFunction *out) {
    if (!stmt) return;
    switch (stmt->kind) {

    case ND_BLOCK:
        for (int i = 0; i < stmt->nchildren; i++) irStmt(stmt->children[i], out);
        break;

    case ND_VAR_DECL:
        if (stmt->nchildren == 0) break;  // declaration without initialiser
        if (stmt->nchildren == 1) {
            // Scalar initialiser: write directly into the declared variable.
            irExprInto(stmt->children[0], out, mkVar(stmt));
        } else {
            // Array initialiser: one IR_STORE_ARR per element.
            for (int i = 0; i < stmt->nchildren; i++) {
                Operand v = irExpr(stmt->children[i], out);
                emit(out, IR_STORE_ARR, mkVar(stmt), mkConstInt(i), v);
            }
        }
        break;

    case ND_EXPR_STMT:
        irExpr(stmt->children[0], out);
        break;

    case ND_IF: {
        Operand elseLbl = mkLabel();
        irJumpIfFalse(stmt->children[0], out, elseLbl);
        irStmt(stmt->children[1], out);                  // then-branch
        if (stmt->nchildren > 2) {
            // Has else-branch: jump past it after then-branch.
            Operand endLbl = mkLabel();
            emitGoto(out, endLbl);
            emitLabel(out, elseLbl);
            irStmt(stmt->children[2], out);              // else-branch
            emitLabel(out, endLbl);
        } else {
            emitLabel(out, elseLbl);
        }
        break;
    }

    case ND_WHILE: {
        Operand startLbl = mkLabel();
        Operand endLbl   = mkLabel();
        emitLabel(out, startLbl);
        irJumpIfFalse(stmt->children[0], out, endLbl);  // loop exit test
        currentLoopDepth++;                               // body is one level deeper
        irStmt(stmt->children[1], out);
        currentLoopDepth--;
        emitGoto(out, startLbl);                          // back-edge
        emitLabel(out, endLbl);
        break;
    }

    case ND_RETURN: {
        Operand v = irExpr(stmt->children[0], out);
        emit(out, IR_RETURN, noOperand(), v, noOperand());
        break;
    }

    default: break;
    }
}

/* =========================================================================
 * Function compilation
 * ========================================================================= */

/**
 * @brief Compile a single ND_FUNC_DECL node into an optimised IRFunction.
 *
 * Sequence:
 *  1. Emit IR for the function body (irStmt on the ND_BLOCK child).
 *  2. Resolve the CFG (resolveCFG).
 *  3. Run the optimisation pipeline:
 *       SVN (value numbering)
 *     → DCE (dead-code elimination)
 *     → loop: CP (constant propagation + CFG pruning) + DCE until stable
 *     → LICM (loop-invariant code motion) + SR (strength reduction)
 *     → if anything changed: another CP + DCE loop
 *
 * @param decl ND_FUNC_DECL AST node.
 * @return     Heap-allocated, optimised IRFunction.
 */
static IRFunction *irFunction(ASTNode *decl) {
    // Extract function name from "returnType name" text field.
    const char *space = strrchr(decl->text, ' ');
    const char *name  = space ? space + 1 : decl->text;

    IRFunction *f = calloc(1, sizeof(IRFunction));
    f->name          = strdup(name);
    f->labelBase     = nextLabel;
    f->curBlockStart = 0;
    currentLoopDepth = 0;

    // Emit IR for the function body (last child = ND_BLOCK).
    ASTNode *body = decl->children[decl->nchildren - 1];
    irStmt(body, f);

    // Build and resolve the CFG from the flat instruction array.
    resolveCFG(f);

    // --- Optimisation pipeline -----------------------------------------

    // SVN: eliminate redundant re-computations along EBB paths.
    svn_optimize(f);

    // Initial DCE: remove trivially dead code after SVN.
    dce_optimize(f);

    // CP + DCE: iterate to a fixed point (constant propagation may expose
    // new dead code, which in turn may expose new constants).
    int changed;
    do {
        changed  = cp_optimize(f);
        changed |= dce_optimize(f);
    } while (changed);

    // LICM + SR: loop transformations (require the pre-header inserted by LICM).
    changed  = licm_optimize(f);
    changed |= sr_optimize(f);

    // If loop passes modified the IR, clean up with another CP + DCE round.
    if (changed) {
        do {
            changed  = cp_optimize(f);
            changed |= dce_optimize(f);
        } while (changed);
    }

    return f;
}

/* =========================================================================
 * IRProgram management
 * ========================================================================= */

/**
 * @brief Append @p f to @p prog, growing the function array if necessary.
 *
 * @param prog Target program.
 * @param f    Function to append.
 */
static void programPush(IRProgram *prog, IRFunction *f) {
    if (prog->count == prog->capacity) {
        prog->capacity = prog->capacity ? prog->capacity * 2 : 8;
        prog->functions = realloc(prog->functions,
                                  (size_t)prog->capacity * sizeof(IRFunction *));
    }
    prog->functions[prog->count++] = f;
}

/* =========================================================================
 * Public API — ir_generate
 * ========================================================================= */

IRProgram *ir_generate(ASTNode *program) {
    // Reset global counters; temps and labels are unique per translation unit.
    nextTemp  = 0;
    nextLabel = 0;

    IRProgram *prog = calloc(1, sizeof(IRProgram));
    for (int i = 0; i < program->nchildren; i++) {
        ASTNode *decl = program->children[i];
        if (decl->kind != ND_FUNC_DECL) continue;   // skip global var decls
        programPush(prog, irFunction(decl));
    }
    return prog;
}

/* =========================================================================
 * Debug printing
 * ========================================================================= */

/**
 * @brief Print a single operand to stdout (human-readable form).
 *
 * Format:
 *   OPND_TEMP        → tN
 *   OPND_VAR         → vLEVEL.OFFSET[/name]
 *   OPND_CONST_INT   → decimal integer
 *   OPND_CONST_FLOAT → %g float
 *   OPND_LABEL       → LN
 *   OPND_FUNC        → function name string
 *
 * @param o Operand to print.
 */
static void printOperand(const Operand *o) {
    switch (o->kind) {
    case OPND_NONE:        break;
    case OPND_TEMP:        printf("t%d", o->data.tempId); break;
    case OPND_VAR:
        printf("v%d.%d", o->data.varLevel, o->data.varOffset);
        if (o->data.sourceName) printf("/*%s*/", o->data.sourceName);
        break;
    case OPND_CONST_INT:   printf("%d",  o->data.intVal);            break;
    case OPND_CONST_FLOAT: printf("%g",  (double)o->data.floatVal);  break;
    case OPND_LABEL:       printf("L%d", o->data.labelId);           break;
    case OPND_FUNC:        printf("%s",  o->data.funcName);          break;
    }
}

/**
 * @brief Return the infix mnemonic string for a binary/comparison IROp.
 *
 * Only opcodes that appear between two operands need a mnemonic here;
 * unary and control-flow opcodes are handled inline in printInstr.
 *
 * @param op Binary or comparison opcode.
 * @return   C-string mnemonic (e.g. "+", "<=").
 */
static const char *opMnemonic(IROp op) {
    switch (op) {
    case IR_ADD: return "+";  case IR_SUB: return "-";
    case IR_MUL: return "*";  case IR_DIV: return "/"; case IR_MOD: return "%";
    case IR_LT:  return "<";  case IR_LE:  return "<=";
    case IR_GT:  return ">";  case IR_GE:  return ">=";
    case IR_EQ:  return "=="; case IR_NE:  return "!=";
    default:     return "?";
    }
}

/**
 * @brief Print a single IR instruction to stdout, followed by a newline.
 *
 * The output format mirrors the original source semantics to make dumps
 * easy to read during debugging.
 *
 * @param in Instruction to print.
 */
static void printInstr(const IRInstr *in) {
    switch (in->op) {
    // Binary and relational: dst = src1 op src2
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_LT:  case IR_LE:  case IR_GT:  case IR_GE:
    case IR_EQ:  case IR_NE:
        printf("    "); printOperand(&in->dst);
        printf(" = ");  printOperand(&in->src1);
        printf(" %s ", opMnemonic(in->op));
        printOperand(&in->src2);
        break;
    case IR_NEG:
        printf("    "); printOperand(&in->dst);
        printf(" = -"); printOperand(&in->src1);
        break;
    case IR_NOT:
        printf("    "); printOperand(&in->dst);
        printf(" = !"); printOperand(&in->src1);
        break;
    case IR_ASSIGN:
        printf("    "); printOperand(&in->dst);
        printf(" = ");  printOperand(&in->src1);
        break;
    case IR_LOAD_ARR:
        printf("    "); printOperand(&in->dst);
        printf(" = ");  printOperand(&in->src1);
        printf("[");    printOperand(&in->src2);
        printf("]");
        break;
    case IR_STORE_ARR:
        printf("    "); printOperand(&in->dst);
        printf("[");    printOperand(&in->src1);
        printf("] = "); printOperand(&in->src2);
        break;
    case IR_PARAM:
        printf("    param "); printOperand(&in->src1);
        break;
    case IR_CALL:
        printf("    "); printOperand(&in->dst);
        printf(" = call "); printOperand(&in->src1);
        printf(", ");       printOperand(&in->src2);
        break;
    case IR_RETURN:
        printf("    return "); printOperand(&in->src1);
        break;
    case IR_GOTO:
        printf("    goto "); printOperand(&in->dst);
        break;
    case IR_IF_FALSE:
        printf("    if_false "); printOperand(&in->src1);
        printf(" goto ");        printOperand(&in->dst);
        break;
    case IR_LABEL:
        // Labels are printed without indentation to stand out.
        printOperand(&in->dst); printf(":");
        break;
    }
    printf("\n");
}

/* =========================================================================
 * Public API — ir_print, ir_free
 * ========================================================================= */

void ir_print(const IRProgram *prog) {
    for (int i = 0; i < prog->count; i++) {
        IRFunction *f = prog->functions[i];
        printf("funzione %s:\n", f->name);
        for (int j = 0; j < f->count; j++) printInstr(&f->instrs[j]);
        printf("\n");
    }
}

void ir_free(IRProgram *prog) {
    if (!prog) return;
    for (int i = 0; i < prog->count; i++) {
        free(prog->functions[i]->name);
        free(prog->functions[i]->instrs);
        free(prog->functions[i]->blocks);
        free(prog->functions[i]->labelToBlock);  // NULL-safe: free(NULL) is a no-op
        free(prog->functions[i]);
    }
    free(prog->functions);
    free(prog);
}