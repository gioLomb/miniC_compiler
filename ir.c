/**
 * @file ir.c
 * @brief IR generation from the AST and IR-level optimisation pipeline.
 *
 * Key design points:
 *   - mk_var: scopeLevel==0 -> OPND_GLOBAL (expanded later by ir_lower_globals)
 *   - ir_is_pure / ir_defines_dst: include IR_GLOBAL_ADDR
 *   - ir_build_function: calls ir_lower_globals() after ir_resolve_cfg()
 *     and before SVN/DCE/CP/LICM/SR
 *   - ir_build_function: populates f->params/f->paramCount with the formal
 *     parameters' (OPND_VAR) operands, so instr_selector.c can generate the
 *     ABI-register -> vreg binding MOVs at function entry.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "ir.h"
#include "global_lower.h"
#include "svn.h"
#include "dce.h"
#include "cp.h"
#include "licm.h"
#include "sr.h"
#include "sched.h"
#include "arena.h"



static inline unsigned short op_key(const char *s) {
    if (!s || !s[0]) return 0;
    unsigned short k = (unsigned char)s[0] << 8;
    if (s[1]) k |= (unsigned char)s[1];
    return k;
}

// reset once per ir_generate() call; shared across every function compiled
// in that program so temp/label ids are globally unique
static int nextTemp;
static int nextLabel;
// tracks static loop nesting during codegen; stamped onto every emitted
// instruction so later passes (regalloc spill weighting) can prioritise
// hot-loop values without re-deriving loop structure
static int currentLoopDepth;



static inline Operand mk_var(const ASTNode *node) {
    if (node->scopeLevel == 0) {
        return (Operand){ .kind              = OPND_GLOBAL,
                          .data.globalOffset = node->offset };
    }
    return (Operand){ .kind            = OPND_VAR,
                      .data.varLevel   = node->scopeLevel,
                      .data.varOffset  = node->offset,
                      .data.sourceName = node->text };
}


static inline int ir_is_terminator(IROp op) {
    // bitmask trick: terminators always end a basic block
    const unsigned int mask =
        (1U << IR_GOTO) | (1U << IR_IF_FALSE) | (1U << IR_RETURN);
    return (mask & (1U << op)) != 0;
}

/**
 * IR_GLOBAL_ADDR is pure: it only produces an address, no side effects.
 * LICM can hoist it, DCE can eliminate it if the result is dead.
 */
int ir_is_pure(IROp op) {
    static const unsigned int mask =
        (1U << IR_ADD)  | (1U << IR_SUB) | (1U << IR_MUL) |
        (1U << IR_DIV)  | (1U << IR_MOD) | (1U << IR_NEG) |
        (1U << IR_NOT)  | (1U << IR_LT)  | (1U << IR_LE)  |
        (1U << IR_GT)   | (1U << IR_GE)  | (1U << IR_EQ)  |
        (1U << IR_NE)   | (1U << IR_ASSIGN) | (1U << IR_GLOBAL_ADDR);
    return (op < 32) && ((mask >> op) & 1U);
}

/**
 * @brief Compact one block's instruction range in place, keeping only
 *        surviving (non-eliminated) instructions.
 *
 * Safe in-place write: writeCursor never exceeds the read index i, since
 * it only advances on a kept instruction (and always by <= the number of
 * instructions read so far).
 *
 * @param f           Function whose instrs[] is compacted in place.
 * @param eliminate   Per-instruction elimination flags.
 * @param b           Block index being compacted.
 * @param writeCursor In/out: next free slot in f->instrs; advanced by the
 *                     number of surviving instructions in this block.
 */
static void compact_block(IRFunction *f, const char *eliminate, int b, int *writeCursor) {
    int oldStart = f->blocks[b].bb.range.start;
    int oldEnd   = f->blocks[b].bb.range.end;
    int newStart = *writeCursor;

    for (int i = oldStart; i < oldEnd; i++)
        if (!eliminate[i])
            f->instrs[(*writeCursor)++] = f->instrs[i];

    f->blocks[b].bb.range.start = newStart;
    f->blocks[b].bb.range.end   = *writeCursor;
}

int ir_sweep(IRFunction *f, char *eliminate, int nBlocks) {
    int nInstrs = f->count;
    int writeCursor = 0;

    for (int b = 0; b < nBlocks; b++)
        compact_block(f, eliminate, b, &writeCursor);

    f->count         = writeCursor;
    f->capacity      = f->capacity;
    f->curBlockStart = 0;

    int changed = (writeCursor != nInstrs);
    // Instruction indices shifted: any VarMap id cached against the old
    // indices (varmap_sync_cache) is now misaligned and must be rebuilt.
    if (changed) f->ver++;

    return changed;
}

static inline void ir_close_block(IRFunction *f, int start, int end) {
    if (end <= start) return; // empty range: nothing to record
    // standard doubling growth for the block array
    if (f->blockCount == f->blockCap) {
        f->blockCap = f->blockCap ? f->blockCap * 2 : 16;
        f->blocks = realloc(f->blocks, (size_t)f->blockCap * sizeof(IRBlock));
    }
    f->blocks[f->blockCount++] = (IRBlock){
        .bb       = { .range = {.start = start, .end = end}, .succ = {-1, -1} },
        .predCount = 0,
    };
}

static void ir_register_label(IRFunction *f, int labelId, int futureBlockIdx) {
    // labelToBlock is indexed relative to this function's labelBase, since
    // label ids are allocated globally across all functions in the program
    int idx = labelId - f->labelBase;
    if (idx >= f->labelToBlockCap) {
        int newCap = f->labelToBlockCap ? f->labelToBlockCap : 8;
        while (newCap <= idx) newCap <<= 1; // grow until idx fits
        int *newTable = realloc(f->labelToBlock, (size_t)newCap * sizeof(int));
        if (!newTable) { fprintf(stderr, "OOM in ir_register_label\n"); exit(1); }
        f->labelToBlock = newTable;
        // newly grown slots must start as "unresolved" (-1)
        memset(f->labelToBlock + f->labelToBlockCap, -1,
               (size_t)(newCap - f->labelToBlockCap) * sizeof(int));
        f->labelToBlockCap = newCap;
    }
    f->labelToBlock[idx] = futureBlockIdx;
}

static void ir_emit_instr(IRFunction *f, IROp op, Operand dst, Operand src1, Operand src2) {
    // standard doubling growth
    if (__builtin_expect(f->count == f->capacity,0)) {
        f->capacity = f->capacity ? f->capacity * 2 : 16;
        f->instrs = realloc(f->instrs, (size_t)f->capacity * sizeof(IRInstr));
    }
    int idx = f->count;

    // a label always starts a new block: close whatever was open before it
    // (labels are jump targets, so control flow can enter here from elsewhere)
    if (__builtin_expect(op == IR_LABEL,0) && idx > f->curBlockStart) {
        ir_close_block(f, f->curBlockStart, idx);
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

    // record where this label ended up so ir_resolve_cfg can later resolve
    // jump targets by label id -> block index
    if (__builtin_expect(op == IR_LABEL,0)) ir_register_label(f, dst.data.labelId, f->blockCount);

    // a terminator always ends the current block (GOTO/IF_FALSE/RETURN
    // are the last instruction control can reach before branching/exiting)
    if (__builtin_expect(ir_is_terminator(op),0)) {
        ir_close_block(f, f->curBlockStart, f->count);
        f->curBlockStart = f->count;
    }
}

/* =========================================================================
 * CFG resolution
 * ========================================================================= */

static void ir_resolve_cfg(IRFunction *f) {
    // close any trailing block that never hit a terminator or label
    if (f->curBlockStart < f->count) {
        ir_close_block(f, f->curBlockStart, f->count);
        f->curBlockStart = f->count;
    }

    // for every block, derive its successor edges from its last instruction
    for (int b = 0; b < f->blockCount; b++) {
        int last = f->blocks[b].bb.range.end - 1;
        IROp op  = f->instrs[last].op;

        if (op == IR_GOTO) {
            // unconditional jump: single successor resolved via labelToBlock
            int lbl = f->instrs[last].dst.data.labelId - f->labelBase;
            f->blocks[b].bb.succ[0] =
                (lbl >= 0 && lbl < f->labelToBlockCap) ? f->labelToBlock[lbl] : -1;

        } else if (op == IR_IF_FALSE) {
            // two successors: fall-through (condition true) and jump target (condition false)
            f->blocks[b].bb.succ[0] = (b + 1 < f->blockCount) ? b + 1 : -1;
            int lbl = f->instrs[last].dst.data.labelId - f->labelBase;
            f->blocks[b].bb.succ[1] =
                (lbl >= 0 && lbl < f->labelToBlockCap) ? f->labelToBlock[lbl] : -1;

        } else if (op != IR_RETURN) {
            // ordinary fall-through block (no explicit terminator, or the
            // block was closed only because a label follows)
            f->blocks[b].bb.succ[0] = (b + 1 < f->blockCount) ? b + 1 : -1;
        }
        // IR_RETURN: no successors, leaves succ = {-1, -1}
    }

    // derive predCount from the succ[] edges just computed
    for (int b = 0; b < f->blockCount; b++)
        for (int k = 0; k < 2; k++) {
            int s = f->blocks[b].bb.succ[k];
            if (s >= 0) f->blocks[s].predCount++;
        }

    // labelToBlock is only needed during CFG resolution; free it now
    free(f->labelToBlock);
    f->labelToBlock    = NULL;
    f->labelToBlockCap = 0;
}

/* =========================================================================
 * Expression code generation — forward declarations
 * ========================================================================= */

static Operand ir_emit_expr(ASTNode *expr, IRFunction *out);
static Operand ir_emit_expr_into(ASTNode *expr, IRFunction *out, Operand dest);
static void    ir_emit_jump_if_false(ASTNode *cond, IRFunction *out, Operand falseLbl);
static void    ir_emit_jump_if_true (ASTNode *cond, IRFunction *out, Operand trueLbl);

/* =========================================================================
 * Binary operator -> IROp mapping
 * ========================================================================= */

static inline IROp ir_binop_to_irop(const char *op) {
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
    default:             return IR_ADD; // unreachable for well-formed AST
    }
}

/* =========================================================================
 * Short-circuit Boolean code generation
 * ========================================================================= */

/**
 * @brief Emit code that jumps to @p falseLbl iff @p cond evaluates to false,
 *        short-circuiting && and || without ever materialising an intermediate value.
 */
static void ir_emit_jump_if_false(ASTNode *cond, IRFunction *out, Operand falseLbl) {
    if (cond->kind == ND_BINOP && op_key(cond->text) == KEY_AND) {
        // a && b is false as soon as either side is false: jump to falseLbl
        // directly from each side, no combined value ever computed
        ir_emit_jump_if_false(cond->children[0], out, falseLbl);
        ir_emit_jump_if_false(cond->children[1], out, falseLbl);
        return;
    }
    if (cond->kind == ND_BINOP && op_key(cond->text) == KEY_OR) {
        // a || b is false only if BOTH sides are false: if lhs is true skip
        // the rhs check entirely (it can't change the outcome)
        Operand skipLbl = (Operand){ .kind = OPND_LABEL, .data.labelId = nextLabel++ };
        ir_emit_jump_if_true(cond->children[0], out, skipLbl);
        ir_emit_jump_if_false(cond->children[1], out, falseLbl);
        ir_emit_instr(out, IR_LABEL, skipLbl, (Operand){.kind = OPND_NONE}, (Operand){.kind = OPND_NONE});
        return;
    }
    if (cond->kind == ND_UNARY && op_key(cond->text) == KEY_NOT) {
        // !x is false <=> x is true: push the negation into the target label
        ir_emit_jump_if_true(cond->children[0], out, falseLbl);
        return;
    }
    // base case: no further short-circuit structure, evaluate and test
    Operand v = ir_emit_expr(cond, out);
    ir_emit_instr(out, IR_IF_FALSE, falseLbl, v, (Operand){.kind = OPND_NONE});
}

/**
 * @brief Emit code that jumps to @p trueLbl iff @p cond evaluates to true.
 *        Mirror image of ir_emit_jump_if_false.
 */
static void ir_emit_jump_if_true(ASTNode *cond, IRFunction *out, Operand trueLbl) {
    if (cond->kind == ND_BINOP && op_key(cond->text) == KEY_AND) {
        // a && b is true only if BOTH sides are true
        Operand skipLbl = (Operand){ .kind = OPND_LABEL, .data.labelId = nextLabel++ };
        ir_emit_jump_if_false(cond->children[0], out, skipLbl);
        ir_emit_jump_if_true(cond->children[1], out, trueLbl);
        ir_emit_instr(out, IR_LABEL, skipLbl, (Operand){.kind = OPND_NONE}, (Operand){.kind = OPND_NONE});
        return;
    }
    if (cond->kind == ND_BINOP && op_key(cond->text) == KEY_OR) {
        // a || b is true as soon as either side is true
        ir_emit_jump_if_true(cond->children[0], out, trueLbl);
        ir_emit_jump_if_true(cond->children[1], out, trueLbl);
        return;
    }
    if (cond->kind == ND_UNARY && op_key(cond->text) == KEY_NOT) {
        ir_emit_jump_if_false(cond->children[0], out, trueLbl);
        return;
    }
    // base case: no short-circuit operator — evaluate, test, jump manually
    // (IR_IF_FALSE only jumps on false, so a "jump on true" needs an
    // explicit skip-over-the-goto pattern)
    Operand v       = ir_emit_expr(cond, out);
    Operand skipLbl = (Operand){ .kind = OPND_LABEL, .data.labelId = nextLabel++ };
    ir_emit_instr(out, IR_IF_FALSE, skipLbl, v, (Operand){.kind = OPND_NONE});
    ir_emit_instr(out, IR_GOTO, trueLbl, (Operand){.kind = OPND_NONE}, (Operand){.kind = OPND_NONE});
    ir_emit_instr(out, IR_LABEL, skipLbl, (Operand){.kind = OPND_NONE}, (Operand){.kind = OPND_NONE});
}

/**
 * @brief Materialise a boolean expression (possibly &&/||/!) into @p dest as 0 or 1.
 */
static Operand ir_emit_short_circuit_into(ASTNode *expr, IRFunction *out, Operand dest) {
    Operand endLbl   = (Operand){ .kind = OPND_LABEL, .data.labelId = nextLabel++ };
    Operand falseLbl = (Operand){ .kind = OPND_LABEL, .data.labelId = nextLabel++ };
    ir_emit_jump_if_false(expr, out, falseLbl);
    // reached only if expr was true
    ir_emit_instr(out, IR_ASSIGN, dest, (Operand){ .kind = OPND_CONST_INT, .data.intVal = 1 }, (Operand){.kind = OPND_NONE});
    ir_emit_instr(out, IR_GOTO, endLbl, (Operand){.kind = OPND_NONE}, (Operand){.kind = OPND_NONE});
    ir_emit_instr(out, IR_LABEL, falseLbl, (Operand){.kind = OPND_NONE}, (Operand){.kind = OPND_NONE});
    ir_emit_instr(out, IR_ASSIGN, dest, (Operand){ .kind = OPND_CONST_INT, .data.intVal = 0 }, (Operand){.kind = OPND_NONE});
    ir_emit_instr(out, IR_LABEL, endLbl, (Operand){.kind = OPND_NONE}, (Operand){.kind = OPND_NONE});
    return dest;
}

static Operand ir_emit_short_circuit(ASTNode *expr, IRFunction *out) {
    return ir_emit_short_circuit_into(expr, out, (Operand){ .kind = OPND_TEMP, .data.tempId = nextTemp++ });
}

/* =========================================================================
 * Assignment and call helpers
 * ========================================================================= */

static Operand ir_emit_assign(ASTNode *expr, IRFunction *out) {
    ASTNode *lvalue = expr->children[0];
    if (lvalue->kind == ND_ID)
        // simple scalar assignment: emit the rhs directly into the variable's slot
        return ir_emit_expr_into(expr->children[1], out, mk_var(lvalue));

    // array-element assignment: base[idx] = rhs
    Operand idx  = ir_emit_expr(lvalue->children[0], out);
    Operand base = mk_var(lvalue);
    Operand rhs  = ir_emit_expr(expr->children[1], out);
    ir_emit_instr(out, IR_STORE_ARR, base, idx, rhs);
    return rhs; // assignment expression evaluates to the assigned value
}

static Operand ir_emit_call(ASTNode *expr, IRFunction *out) {
    // each argument is pushed via a dedicated IR_PARAM right before the call
    for (int i = 0; i < expr->nchildren; i++) {
        Operand arg = ir_emit_expr(expr->children[i], out);
        ir_emit_instr(out, IR_PARAM, (Operand){.kind = OPND_NONE}, arg, (Operand){.kind = OPND_NONE});
    }
    Operand result = (Operand){ .kind = OPND_TEMP, .data.tempId = nextTemp++ };
    ir_emit_instr(out, IR_CALL, result, (Operand){ .kind = OPND_FUNC, .data.funcName = expr->text }, (Operand){ .kind = OPND_CONST_INT, .data.intVal = expr->nchildren });
    return result;
}

/* =========================================================================
 * ir_emit_expr — evaluate an expression, returning the operand holding its value
 * ========================================================================= */

static Operand ir_emit_expr(ASTNode *expr, IRFunction *out) {
    switch (expr->kind) {
    case ND_NUM_INT:   return (Operand){ .kind = OPND_CONST_INT, .data.intVal = atoi(expr->text) };
    case ND_NUM_FLOAT: return (Operand){ .kind = OPND_CONST_FLOAT, .data.floatVal = (float)atof(expr->text) };
    case ND_ID:        return mk_var(expr);

    case ND_ARRAY_ACCESS: {
        Operand idx  = ir_emit_expr(expr->children[0], out);
        Operand base = mk_var(expr);
        Operand t    = (Operand){ .kind = OPND_TEMP, .data.tempId = nextTemp++ };
        ir_emit_instr(out, IR_LOAD_ARR, t, base, idx);
        return t;
    }

    case ND_UNARY: {
        Operand v = ir_emit_expr(expr->children[0], out);
        Operand t = (Operand){ .kind = OPND_TEMP, .data.tempId = nextTemp++ };
        ir_emit_instr(out, op_key(expr->text) == KEY_NOT ? IR_NOT : IR_NEG, t, v, (Operand){.kind = OPND_NONE});
        return t;
    }

    case ND_BINOP: {
        unsigned short key = op_key(expr->text);
        // && and || need short-circuit control flow, not a plain binop
        if (key == KEY_AND || key == KEY_OR)
            return ir_emit_short_circuit(expr, out);
        Operand lhs = ir_emit_expr(expr->children[0], out);
        Operand rhs = ir_emit_expr(expr->children[1], out);
        Operand t   = (Operand){ .kind = OPND_TEMP, .data.tempId = nextTemp++ };
        ir_emit_instr(out, ir_binop_to_irop(expr->text), t, lhs, rhs);
        return t;
    }

    case ND_ASSIGN: return ir_emit_assign(expr, out);
    case ND_CALL:   return ir_emit_call(expr, out);
    default:        return (Operand){.kind = OPND_NONE}; // ND_ERROR or unexpected node
    }
}

/**
 * @brief Same as ir_emit_expr, but writes the result directly into @p dest
 *        instead of allocating a fresh temporary.
 *
 * Used wherever the destination is already known (assignments, declaration
 * initializers) so the pipeline avoids an extra "temp = expr; var = temp"
 * copy that SVN/CP would otherwise have to clean up.
 */
static Operand ir_emit_expr_into(ASTNode *expr, IRFunction *out, Operand dest) {
    switch (expr->kind) {
    case ND_NUM_INT:
        ir_emit_instr(out, IR_ASSIGN, dest, (Operand){ .kind = OPND_CONST_INT, .data.intVal = atoi(expr->text) }, (Operand){.kind = OPND_NONE});
        return dest;
    case ND_NUM_FLOAT:
        ir_emit_instr(out, IR_ASSIGN, dest, (Operand){ .kind = OPND_CONST_FLOAT, .data.floatVal = (float)atof(expr->text) }, (Operand){.kind = OPND_NONE});
        return dest;
    case ND_ID:
        ir_emit_instr(out, IR_ASSIGN, dest, mk_var(expr), (Operand){.kind = OPND_NONE});
        return dest;

    case ND_ARRAY_ACCESS: {
        Operand idx  = ir_emit_expr(expr->children[0], out);
        Operand base = mk_var(expr);
        ir_emit_instr(out, IR_LOAD_ARR, dest, base, idx);
        return dest;
    }

    case ND_UNARY: {
        Operand v = ir_emit_expr(expr->children[0], out);
        ir_emit_instr(out, op_key(expr->text) == KEY_NOT ? IR_NOT : IR_NEG, dest, v, (Operand){.kind = OPND_NONE});
        return dest;
    }

    case ND_BINOP: {
        unsigned short key = op_key(expr->text);
        if (key == KEY_AND || key == KEY_OR)
            return ir_emit_short_circuit_into(expr, out, dest);
        Operand lhs = ir_emit_expr(expr->children[0], out);
        Operand rhs = ir_emit_expr(expr->children[1], out);
        ir_emit_instr(out, ir_binop_to_irop(expr->text), dest, lhs, rhs);
        return dest;
    }

    case ND_CALL: {
        for (int i = 0; i < expr->nchildren; i++) {
            Operand arg = ir_emit_expr(expr->children[i], out);
            ir_emit_instr(out, IR_PARAM, (Operand){.kind = OPND_NONE}, arg, (Operand){.kind = OPND_NONE});
        }
        // unlike ir_emit_call, the CALL result is written straight into dest
        ir_emit_instr(out, IR_CALL, dest, (Operand){ .kind = OPND_FUNC, .data.funcName = expr->text }, (Operand){ .kind = OPND_CONST_INT, .data.intVal = expr->nchildren });
        return dest;
    }

    case ND_ASSIGN: {
        // "x = (y = z)": inner assignment computes its own target, then the
        // resulting value is additionally copied into dest for this context
        Operand inner = ir_emit_assign(expr, out);
        ir_emit_instr(out, IR_ASSIGN, dest, inner, (Operand){.kind = OPND_NONE});
        return dest;
    }

    default: return (Operand){.kind = OPND_NONE};
    }
}

/* =========================================================================
 * Statement code generation
 * ========================================================================= */

static void ir_emit_stmt(ASTNode *stmt, IRFunction *out) {
    if (!stmt) return;
    switch (stmt->kind) {

    case ND_BLOCK:
        for (int i = 0; i < stmt->nchildren; i++) ir_emit_stmt(stmt->children[i], out);
        break;

    case ND_VAR_DECL:
        if (stmt->nchildren == 0) break; // no initializer: nothing to emit
        if (stmt->nchildren == 1) {
            // scalar initializer
            ir_emit_expr_into(stmt->children[0], out, mk_var(stmt));
        } else {
            // array initializer list: one STORE_ARR per element, index = position
            for (int i = 0; i < stmt->nchildren; i++) {
                Operand v = ir_emit_expr(stmt->children[i], out);
                ir_emit_instr(out, IR_STORE_ARR, mk_var(stmt), (Operand){ .kind = OPND_CONST_INT, .data.intVal = i }, v);
            }
        }
        break;

    case ND_EXPR_STMT:
        ir_emit_expr(stmt->children[0], out); // value discarded, side effects only
        break;

    case ND_IF: {
        Operand elseLbl = (Operand){ .kind = OPND_LABEL, .data.labelId = nextLabel++ };
        ir_emit_jump_if_false(stmt->children[0], out, elseLbl);
        ir_emit_stmt(stmt->children[1], out); // then-branch
        if (stmt->nchildren > 2) {
            // has an else-branch: then-branch must skip over it
            Operand endLbl = (Operand){ .kind = OPND_LABEL, .data.labelId = nextLabel++ };
            ir_emit_instr(out, IR_GOTO, endLbl, (Operand){.kind = OPND_NONE}, (Operand){.kind = OPND_NONE});
            ir_emit_instr(out, IR_LABEL, elseLbl, (Operand){.kind = OPND_NONE}, (Operand){.kind = OPND_NONE});
            ir_emit_stmt(stmt->children[2], out);
            ir_emit_instr(out, IR_LABEL, endLbl, (Operand){.kind = OPND_NONE}, (Operand){.kind = OPND_NONE});
        } else {
            // no else-branch: elseLbl doubles as the join point
            ir_emit_instr(out, IR_LABEL, elseLbl, (Operand){.kind = OPND_NONE}, (Operand){.kind = OPND_NONE});
        }
        break;
    }

    case ND_WHILE: {
        Operand startLbl = (Operand){ .kind = OPND_LABEL, .data.labelId = nextLabel++ };
        Operand endLbl   = (Operand){ .kind = OPND_LABEL, .data.labelId = nextLabel++ };
        ir_emit_instr(out, IR_LABEL, startLbl, (Operand){.kind = OPND_NONE}, (Operand){.kind = OPND_NONE});
        ir_emit_jump_if_false(stmt->children[0], out, endLbl);
        currentLoopDepth++;   // body instructions are nested one level deeper
        ir_emit_stmt(stmt->children[1], out);
        currentLoopDepth--;
        ir_emit_instr(out, IR_GOTO, startLbl, (Operand){.kind = OPND_NONE}, (Operand){.kind = OPND_NONE}); // loop back to re-check the condition
        ir_emit_instr(out, IR_LABEL, endLbl, (Operand){.kind = OPND_NONE}, (Operand){.kind = OPND_NONE});
        break;
    }

    case ND_RETURN: {
        Operand v = ir_emit_expr(stmt->children[0], out);
        ir_emit_instr(out, IR_RETURN, (Operand){.kind = OPND_NONE}, v, (Operand){.kind = OPND_NONE});
        break;
    }

    default: break; // ND_FUNC_DECL/ND_PARAM never appear as statements; ND_ERROR ignored
    }
}

/* =========================================================================
 * Function compilation
 * ========================================================================= */

static IRFunction *ir_build_function(ASTNode *decl,Arena *arena) {
    // decl->text is "returnType funcName"; the name is everything after the last space
    const char *space = strrchr(decl->text, ' ');
    const char *name  = space ? space + 1 : decl->text;

    IRFunction *f = calloc(1, sizeof(IRFunction));
    f->name          = strdup(name);
    f->labelBase     = nextLabel; // labels for this function start where the last one left off
    f->curBlockStart = 0;
    currentLoopDepth = 0;

    /* All children except the last are ND_PARAM; the last is the body.
     * st_bind_symbol (called during semantic_check) already stamped
     * scopeLevel/offset on every ND_PARAM, so mk_var() here produces the
     * correct OPND_VAR (scopeLevel>0: never OPND_GLOBAL for a parameter). */
    int paramCount = decl->nchildren - 1;
    f->paramCount  = paramCount;
    f->params      = paramCount > 0 ? malloc((size_t)paramCount * sizeof(Operand)) : NULL;
    for (int p = 0; p < paramCount; p++)
        f->params[p] = mk_var(decl->children[p]);

    ASTNode *body = decl->children[decl->nchildren - 1];
    ir_emit_stmt(body, f);

    ir_resolve_cfg(f);

       ir_lower_globals(f,arena);

    svn_optimize(f);

    // Single VarMap shared by every cp_optimize/dce_optimize call for the
    // whole pipeline of this function. Its per-instruction id cache
    // (varmap_sync_cache, called internally by cp/dce) is rebuilt only
    // when f->ver changes, instead of every call re-hashing every operand.
    VarMap sharedVarMap = varmap_init();

    dce_optimize(f, &sharedVarMap, arena);

    int changed;
    do {
        changed  = cp_optimize(f, &sharedVarMap, arena);
        changed |= dce_optimize(f, &sharedVarMap, arena);
    } while (changed);

    changed  = licm_optimize(f,arena);
    changed |= sr_optimize(f,arena);

    if (changed) {
        do {
            changed  = cp_optimize(f, &sharedVarMap, arena);
            changed |= dce_optimize(f, &sharedVarMap, arena);
        } while (changed);
    }

    varmap_destroy(&sharedVarMap);

    return f;
}

/* =========================================================================
 * IRProgram management
 * ========================================================================= */

static void ir_program_append(IRProgram *prog, IRFunction *f) {
    // standard doubling growth
    if (prog->count == prog->capacity) {
        prog->capacity = prog->capacity ? prog->capacity * 2 : 8;
        prog->functions = realloc(prog->functions,
                                  (size_t)prog->capacity * sizeof(IRFunction *));
    }
    prog->functions[prog->count++] = f;
}
/**
 * @brief Parsed view of a global declaration's textual encoding.
 *
 * tyName/name point into the caller-owned mutable buffer passed to
 * ir_parse_global_decl_text, so they stay valid only as long as that
 * buffer does.
 */
typedef struct {
    char *tyName;
    char *name;
    int   isArray;
    int   arraySize;
} GlobalDeclInfo;

/**
 * @brief Parse buf ("type name" or "type name[size]") in place into its
 *        type/name/array-size components (same convention as ast_to_symtab.c).
 *
 * @param buf  Mutable copy of decl->text; NUL bytes are inserted at the
 *             type/name and name/'[' boundaries, so out->tyName/out->name
 *             end up pointing into it.
 * @return 1 on success, 0 if the text is malformed (no space separator).
 */
static int ir_parse_global_decl_text(char *buf, GlobalDeclInfo *out) {
    char *space = strchr(buf, ' ');
    if (!space) return 0; // malformed text: caller skips defensively
    *space      = '\0';
    out->tyName = buf;
    char *rest  = space + 1;

    out->isArray   = 0;
    out->arraySize = 0;
    char *bracket = strchr(rest, '[');
    if (bracket) {
        *bracket       = '\0';
        out->name      = rest;
        out->isArray   = 1;
        out->arraySize = atoi(bracket + 1);
    } else {
        out->name = rest;
    }
    return 1;
}

static inline DataType ir_global_data_type(const char *tyName) {
    if      (tyName[0] == 'i') return T_INT;
    else if (tyName[0] == 'f') return T_FLOAT;
    return T_VOID;
}

/**
 * @brief Grow prog->globals if needed (standard doubling growth) and
 *        return a pointer to the freshly appended slot.
 */
static IRGlobalVar *ir_globals_append_slot(IRProgram *prog) {
    if (prog->globalCount == prog->globalCap) {
        prog->globalCap = prog->globalCap ? prog->globalCap * 2 : 8;
        prog->globals   = realloc(prog->globals,
                                  (size_t)prog->globalCap * sizeof(IRGlobalVar));
    }
    return &prog->globals[prog->globalCount++];
}

/**
 * @brief Pre-evaluate a global's constant initializer list into a flat
 *        array of longs (int value, or float re-interpreted as raw bits,
 *        so a single 'long' array can hold both int and float initializers).
 *
 * @param decl  ND_VAR_DECL node whose children (if any) are the initializers.
 * @param gv    Global var to populate (initVals/initCount left untouched
 *              if decl has no children).
 */
static void ir_build_global_init_vals(ASTNode *decl, IRGlobalVar *gv) {
    if (decl->nchildren == 0) return; // no initializer: nothing to build

    // has an initializer list: pre-evaluate each constant child into
    // initVals (int value, or float re-interpreted as raw bits so a
    // single 'long' array can hold both int and float initializers)
    int cnt       = decl->nchildren;
    gv->initVals  = malloc((size_t)cnt * sizeof(long));
    gv->initCount = cnt;
    for (int j = 0; j < cnt; j++) {
        ASTNode *ch = decl->children[j];
        if (ch->kind == ND_NUM_INT) {
            gv->initVals[j] = atol(ch->text);
        } else if (ch->kind == ND_NUM_FLOAT) {
            float fv = (float)atof(ch->text);
            long  lv;
            memcpy(&lv, &fv, sizeof fv); // reinterpret bits, not value
            gv->initVals[j] = lv;
        } else {
            gv->initVals[j] = 0; // non-literal initializer: not supported, default 0
        }
    }
}

static void ir_add_global(IRProgram *prog, ASTNode *decl, int symOffset) {
    if (!decl || decl->kind != ND_VAR_DECL) return;

    // decl->text encodes "type name" or "type name[size]"; parse it
    // in-place on a mutable copy (same convention as ast_to_symtab.c)
    char *buf = strdup(decl->text);
    GlobalDeclInfo info;
    if (!ir_parse_global_decl_text(buf, &info)) { free(buf); return; } // malformed text: skip defensively

    IRGlobalVar *gv = ir_globals_append_slot(prog);
    gv->name        = strdup(info.name);
    gv->dataType    = ir_global_data_type(info.tyName);
    gv->isArray     = info.isArray;
    gv->arraySize   = info.arraySize;
    gv->symOffset   = symOffset;
    gv->initVals    = NULL;
    gv->initCount   = 0;

    ir_build_global_init_vals(decl, gv);

    free(buf);
}



IRProgram *ir_generate(ASTNode *program) {
    nextTemp  = 0;
    nextLabel = 0;

    IRProgram *prog = calloc(1, sizeof(IRProgram));
    Arena *arena = arena_create(0);

    // pass 1: register every global variable with a sequential symOffset.
    // Both ND_VAR_DECL and ND_FUNC_DECL advance the counter so symOffset
    // stays in sync with st_resolve_global_namespace's assignment order.
    int symOffset = 0;
    for (int i = 0; i < program->nchildren; i++) {
        ASTNode *decl = program->children[i];
        if (decl->kind == ND_VAR_DECL)
            ir_add_global(prog, decl, symOffset);
        if (decl->kind == ND_VAR_DECL || decl->kind == ND_FUNC_DECL)
            symOffset++;
    }

    // pass 2: compile every function body (globals must all be registered
    // first, since function bodies may reference any global, including
    // ones declared later in the source file)
    for (int i = 0; i < program->nchildren; i++) {
        ASTNode *decl = program->children[i];
        if (decl->kind == ND_FUNC_DECL)
            ir_program_append(prog, ir_build_function(decl,arena));
    }
    
    arena_destroy(arena);
    return prog;
}

/* =========================================================================
 * Debug printing
 * ========================================================================= */

static void ir_print_operand(const Operand *o) {
    switch (o->kind) {
    case OPND_NONE:        break;
    case OPND_TEMP:        printf("t%d", o->data.tempId); break;
    case OPND_VAR:
        printf("v%d.%d", o->data.varLevel, o->data.varOffset);
        if (o->data.sourceName) printf("/*%s*/", o->data.sourceName); // debug hint only
        break;
    case OPND_GLOBAL:      printf("g%d", o->data.globalOffset); break;
    case OPND_CONST_INT:   printf("%d",  o->data.intVal);            break;
    case OPND_CONST_FLOAT: printf("%g",  (double)o->data.floatVal);  break;
    case OPND_LABEL:       printf("L%d", o->data.labelId);           break;
    case OPND_FUNC:        printf("%s",  o->data.funcName);          break;
    }
}

static const char *ir_op_mnemonic(IROp op) {
    switch (op) {
    case IR_ADD: return "+";  case IR_SUB: return "-";
    case IR_MUL: return "*";  case IR_DIV: return "/"; case IR_MOD: return "%";
    case IR_LT:  return "<";  case IR_LE:  return "<=";
    case IR_GT:  return ">";  case IR_GE:  return ">=";
    case IR_EQ:  return "=="; case IR_NE:  return "!=";
    default:     return "?";
    }
}

static void ir_print_instr(const IRInstr *in) {
    switch (in->op) {
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_LT:  case IR_LE:  case IR_GT:  case IR_GE:
    case IR_EQ:  case IR_NE:
        printf("    "); ir_print_operand(&in->dst);
        printf(" = ");  ir_print_operand(&in->src1);
        printf(" %s ", ir_op_mnemonic(in->op));
        ir_print_operand(&in->src2);
        break;
    case IR_NEG:
        printf("    "); ir_print_operand(&in->dst);
        printf(" = -"); ir_print_operand(&in->src1);
        break;
    case IR_NOT:
        printf("    "); ir_print_operand(&in->dst);
        printf(" = !"); ir_print_operand(&in->src1);
        break;
    case IR_ASSIGN:
        printf("    "); ir_print_operand(&in->dst);
        printf(" = ");  ir_print_operand(&in->src1);
        break;
    case IR_GLOBAL_ADDR:
        printf("    "); ir_print_operand(&in->dst);
        printf(" = &g%d", in->src1.data.globalOffset);
        break;
    case IR_LOAD_ARR:
        printf("    "); ir_print_operand(&in->dst);
        printf(" = ");  ir_print_operand(&in->src1);
        printf("[");    ir_print_operand(&in->src2);
        printf("]");
        break;
    case IR_STORE_ARR:
        printf("    "); ir_print_operand(&in->dst);
        printf("[");    ir_print_operand(&in->src1);
        printf("] = "); ir_print_operand(&in->src2);
        break;
    case IR_PARAM:
        printf("    param "); ir_print_operand(&in->src1);
        break;
    case IR_CALL:
        printf("    "); ir_print_operand(&in->dst);
        printf(" = call "); ir_print_operand(&in->src1);
        printf(", ");       ir_print_operand(&in->src2);
        break;
    case IR_RETURN:
        printf("    return "); ir_print_operand(&in->src1);
        break;
    case IR_GOTO:
        printf("    goto "); ir_print_operand(&in->dst);
        break;
    case IR_IF_FALSE:
        printf("    if_false "); ir_print_operand(&in->src1);
        printf(" goto ");        ir_print_operand(&in->dst);
        break;
    case IR_LABEL:
        ir_print_operand(&in->dst); printf(":");
        break;
    }
    printf("\n");
}

void ir_print(const IRProgram *prog) {
    if (prog->globalCount > 0) {
        printf("=== GLOBALI ===\n");
        for (int i = 0; i < prog->globalCount; i++) {
            const IRGlobalVar *g = &prog->globals[i];
            printf("  [offset=%d] %s %s%s",
                   g->symOffset,
                   g->dataType == T_INT ? "int" : "float",
                   g->name,
                   g->isArray ? "[]" : "");
            if (g->initCount > 0) {
                printf(" = {");
                for (int j = 0; j < g->initCount; j++) {
                    if (j) printf(", ");
                    printf("%ld", g->initVals[j]);
                }
                printf("}");
            }
            printf("\n");
        }
        printf("\n");
    }

    for (int i = 0; i < prog->count; i++) {
        IRFunction *f = prog->functions[i];
        printf("funzione %s:\n", f->name);
        for (int j = 0; j < f->count; j++) ir_print_instr(&f->instrs[j]);
        printf("\n");
    }
}

/* =========================================================================
 * Public API — ir_free
 * ========================================================================= */

void ir_free(IRProgram *prog) {
    if (!prog) return;
    for (int i = 0; i < prog->count; i++) {
        free(prog->functions[i]->name);
        free(prog->functions[i]->instrs);
        free(prog->functions[i]->blocks);
        free(prog->functions[i]->labelToBlock);
        free(prog->functions[i]->params);
        free(prog->functions[i]);
    }
    free(prog->functions);

    for (int i = 0; i < prog->globalCount; i++) {
        free(prog->globals[i].name);
        free(prog->globals[i].initVals);
    }
    free(prog->globals);

    free(prog);
}


int ir_defines_dst(IROp op) {
    // opcodes that write a value into dst; used by liveness/DCE/CP to know
    // which instructions produce a trackable definition
    static const uint32_t DEFINES_DST_MASK =
        (1u << IR_ADD)         | (1u << IR_SUB)  | (1u << IR_MUL)  |
        (1u << IR_DIV)         | (1u << IR_MOD)  | (1u << IR_NEG)  |
        (1u << IR_NOT)         | (1u << IR_LT)   | (1u << IR_LE)   |
        (1u << IR_GT)          | (1u << IR_GE)   | (1u << IR_EQ)   |
        (1u << IR_NE)          | (1u << IR_ASSIGN)                  |
        (1u << IR_GLOBAL_ADDR) | (1u << IR_LOAD_ARR) | (1u << IR_CALL);

    return (op < 32) && ((DEFINES_DST_MASK >> op) & 1u);
}

int ir_is_commutative(IROp op) {
    static const unsigned int mask =
        (1U << IR_ADD) | (1U << IR_MUL) | (1U << IR_EQ) | (1U << IR_NE);
    return (mask >> op) & 1U;
}

int ir_operand_is_storage(OperandKind kind) {
    return kind == OPND_VAR || kind == OPND_TEMP;
}


int ir_is_same_operand(const Operand *a, const Operand *b) {
    if (a->kind != b->kind) return 0;
    switch (a->kind) {
    case OPND_VAR:         return a->data.varLevel  == b->data.varLevel &&
                                  a->data.varOffset == b->data.varOffset;
    case OPND_TEMP:        return a->data.tempId    == b->data.tempId;
    case OPND_CONST_INT:   return a->data.intVal    == b->data.intVal;
    case OPND_CONST_FLOAT: return a->data.floatVal  == b->data.floatVal;
    case OPND_LABEL:       return a->data.labelId   == b->data.labelId;
    case OPND_FUNC:        return strcmp(a->data.funcName, b->data.funcName) == 0;
    case OPND_NONE:        return 1;
    default:               return 0;
    }
}

/**
 * @brief Pass 1: count incoming edges per block.
 *
 * One flat scan over every succ[] slot of every block — no per-block
 * search, hence O(nBlocks) rather than the O(nBlocks^2) "for each block,
 * scan all others for a matching succ[]" pattern this replaces.
 *
 * @param f          Function whose blocks[].bb.succ[] is scanned.
 * @param n          f->blockCount, passed in to avoid recomputing it.
 * @param predCount  Out: predCount[b] = number of edges into block b.
 *                   Must already be zero-initialised.
 */
static void ir_count_pred_edges(IRFunction *f, int n, int *predCount) {
    for (int b = 0; b < n; b++)
        for (int k = 0; k < 2; k++) {
            int s = f->blocks[b].bb.succ[k];
            if (s >= 0 && s < n) predCount[s]++;
        }
}

/**
 * @brief Pass 2: turn per-block predecessor counts into a prefix sum,
 *        giving each block a contiguous offset into predData.
 *
 * @param n          Number of blocks.
 * @param predCount  In: predCount[b] = number of predecessors of block b.
 * @param predStart  Out: predStart[b] = running total of predecessors of
 *                   all blocks before b.
 * @return Total number of edges (== sum of predCount[]), i.e. the required
 *         size of predData.
 */
static int ir_prefix_sum_pred_counts(int n, const int *predCount, int *predStart) {
    int total = 0;
    for (int b = 0; b < n; b++) {
        predStart[b] = total;
        total += predCount[b];
    }
    return total;
}

/**
 * @brief Pass 3: scatter each edge into predData at its block's slot,
 *        using a per-block write cursor seeded from predStart.
 *
 * @param f          Function whose blocks[].bb.succ[] is scanned again.
 * @param n          Number of blocks.
 * @param predStart  predStart[b] = start offset of block b's slice in predData.
 * @param predData   Out: flat array of predecessor block indices, one
 *                   contiguous slice per block per predStart/predCount.
 * @param arena      Scratch allocator for the per-block write cursor
 *                   (pure scratch, discarded once this function returns).
 */
static void ir_fill_pred_data(IRFunction *f, int n, const int *predStart, int *predData, Arena *arena) {
    // cursor is pure scratch, discarded after this loop
    int *cursor = arena_alloc(arena, (size_t)n * sizeof(int));
    memcpy(cursor, predStart, (size_t)n * sizeof(int));

    for (int b = 0; b < n; b++)
        for (int k = 0; k < 2; k++) {
            int s = f->blocks[b].bb.succ[k];
            if (s >= 0 && s < n) predData[cursor[s]++] = b;
        }
}

PredList ir_build_pred_list(IRFunction *f, Arena *arena) {
    int n = f->blockCount;
    PredList pl;
    pl.predStart = arena_alloc(arena, (size_t)n * sizeof(int));
    pl.predCount = arena_alloc(arena, (size_t)n * sizeof(int));
    memset(pl.predCount, 0, (size_t)n * sizeof(int));

    ir_count_pred_edges(f, n, pl.predCount);
    int total = ir_prefix_sum_pred_counts(n, pl.predCount, pl.predStart);

    // guard against a zero-size allocation when the function has no edges
    // at all (e.g. a single-block function with no branches)
    pl.predData = arena_alloc(arena, (size_t)(total > 0 ? total : 1) * sizeof(int));

    ir_fill_pred_data(f, n, pl.predStart, pl.predData, arena);

    return pl;
}