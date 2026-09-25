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

static void ir_emit_instr(IRFunction *f, IROp op, Operand dst, Operand src1, Operand src2);
static int  ir_func_returns_float(const ASTNode *decl);

static inline Operand ir_mk_var(const ASTNode *node) {
    // Array BASE addresses (from ND_ARRAY_ACCESS) are always integer even when
    // the element type is float.  For every other storage node (ND_ID, ND_PARAM,
    // ND_VAR_DECL) the isFloat flag follows the resolved dataType so that
    // instr_selector can dispatch to the SSE path.
    int isF = (node->kind != ND_ARRAY_ACCESS) && (node->dataType == T_FLOAT);
    if (node->scopeLevel == 0)
        return (Operand){ .kind = OPND_GLOBAL, .isFloat = isF,
                          .data.globalOffset = node->offset };
    return (Operand){ .kind = OPND_VAR, .isFloat = isF,
                      .data.varLevel = node->scopeLevel,
                      .data.varOffset = node->offset,
                      .data.sourceName = node->text };
}

/* Set while building a function body; used by ND_RETURN to know if int→float
 * widening is required on the returned value. */
static int g_func_returns_float;

/**
 * @brief Ensure @p v is a float-typed operand, emitting IR_ITOF if needed.
 *
 * Integer constants are converted at IR-generation time (no instruction).
 * Already-float operands are returned unchanged.
 */
static Operand ir_ensure_float(Operand v, IRFunction *out) {
    if (v.isFloat || v.kind == OPND_CONST_FLOAT)
        return v;
    if (v.kind == OPND_CONST_INT) {
        return (Operand){ .kind = OPND_CONST_FLOAT, .isFloat = 1,
                          .data.floatVal = (float)v.data.intVal };
    }
    Operand t = { .kind = OPND_TEMP, .isFloat = 1, .data.tempId = nextTemp++ };
    ir_emit_instr(out, IR_ITOF, t, v, (Operand){ .kind = OPND_NONE });
    return t;
}


/* ir_is_terminator / ir_is_pure / … live in ir_op_info.c */

/* ir_compact_block removed: ir_sweep now compacts in instruction order. */
int ir_sweep(IRFunction *f, char *eliminate, int nBlocks) {
    int nInstrs = f->count;
    // Map every old instruction index (and the exclusive-end sentinel
    // nInstrs) to its position in the compacted array. Compacting in
    // *instruction* order — not block-index order — is required because
    // LICM/SR pre-header blocks are appended to f->blocks[] while their
    // instruction ranges sit in the middle of instrs[]. Sweeping by
    // block index would copy those ranges last, after earlier blocks had
    // already overwritten them in place, duplicating labels and leaving
    // hoisted/SR init code past the function's RET.
    int *newPos = malloc((size_t)(nInstrs + 1) * sizeof(int));
    if (!newPos) { ec_report("OOM in ir_sweep\n"); exit(1); }

    int write = 0;
    for (int i = 0; i < nInstrs; i++) {
        newPos[i] = write;
        if (!eliminate[i])
            f->instrs[write++] = f->instrs[i];
    }
    newPos[nInstrs] = write;

    for (int b = 0; b < nBlocks; b++) {
        int s = f->blocks[b].bb.range.start;
        int e = f->blocks[b].bb.range.end;
        if (s < 0) s = 0;
        if (e < 0) e = 0;
        if (s > nInstrs) s = nInstrs;
        if (e > nInstrs) e = nInstrs;
        f->blocks[b].bb.range.start = newPos[s];
        f->blocks[b].bb.range.end   = newPos[e];
    }

    int changed = (write != nInstrs);
    f->count         = write;
    f->curBlockStart = 0;
    if (changed) f->ver++;

    free(newPos);
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
        if (!newTable) { ec_report("OOM in ir_register_label\n"); exit(1); }
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
        return ir_emit_expr_into(expr->children[1], out, ir_mk_var(lvalue));

    // array-element assignment: base[idx] = rhs
    Operand idx  = ir_emit_expr(lvalue->children[0], out);
    Operand base = ir_mk_var(lvalue);
    Operand rhs  = ir_emit_expr(expr->children[1], out);
    // widen int → float when the array element type is float
    if (lvalue->dataType == T_FLOAT)
        rhs = ir_ensure_float(rhs, out);
    ir_emit_instr(out, IR_STORE_ARR, base, idx, rhs);
    return rhs; // assignment expression evaluates to the assigned value
}

static Operand ir_emit_call(ASTNode *expr, IRFunction *out) {
    // Evaluate every argument to completion BEFORE emitting any IR_PARAM.
    // Nested calls in an argument list would otherwise interleave their
    // own PARAM/CALL sequence with the outer call's PARAMs, so the inner
    // CALL consumed the wrong arity/operands (e.g. ackermann_like(m-1,
    // ackermann_like(m, n-1))).
    int n = expr->nchildren;
    Operand *args = n > 0 ? malloc((size_t)n * sizeof(Operand)) : NULL;
    for (int i = 0; i < n; i++)
        args[i] = ir_emit_expr(expr->children[i], out);
    for (int i = 0; i < n; i++)
        ir_emit_instr(out, IR_PARAM, (Operand){.kind = OPND_NONE}, args[i], (Operand){.kind = OPND_NONE});
    free(args);
    Operand result = (Operand){ .kind = OPND_TEMP, .isFloat = (expr->dataType == T_FLOAT), .data.tempId = nextTemp++ };
    ir_emit_instr(out, IR_CALL, result, (Operand){ .kind = OPND_FUNC, .data.funcName = expr->text }, (Operand){ .kind = OPND_CONST_INT, .data.intVal = expr->nchildren });
    return result;
}


static Operand ir_emit_expr(ASTNode *expr, IRFunction *out) {
    switch (expr->kind) {
    case ND_NUM_INT:   return (Operand){ .kind = OPND_CONST_INT, .data.intVal = atoi(expr->text) };
    case ND_NUM_FLOAT: return (Operand){ .kind = OPND_CONST_FLOAT, .isFloat = 1, .data.floatVal = (float)atof(expr->text) };
    case ND_ID:        return ir_mk_var(expr);

    case ND_ARRAY_ACCESS: {
        Operand idx  = ir_emit_expr(expr->children[0], out);
        Operand base = ir_mk_var(expr);
        Operand t = (Operand){ .kind = OPND_TEMP, .isFloat = (expr->dataType == T_FLOAT),
                               .data.tempId = nextTemp++ };
        ir_emit_instr(out, IR_LOAD_ARR, t, base, idx);
        return t;
    }

    case ND_UNARY: {
        Operand v = ir_emit_expr(expr->children[0], out);
        int isF = (expr->dataType == T_FLOAT) || v.isFloat || v.kind == OPND_CONST_FLOAT;
        if (op_key(expr->text) == KEY_NOT) isF = 0; // ! always yields int
        if (isF)
            v = ir_ensure_float(v, out);
        Operand t = (Operand){ .kind = OPND_TEMP, .isFloat = isF,
                               .data.tempId = nextTemp++ };
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
        // Prefer the AST's stamped type; fall back to operand flags because
        // the AST optimiser may rebuild binop nodes without copying dataType.
        int isF = (expr->dataType == T_FLOAT) || lhs.isFloat || rhs.isFloat
                  || lhs.kind == OPND_CONST_FLOAT || rhs.kind == OPND_CONST_FLOAT;
        int isRel = (strchr("=!&|<>", expr->text[0]) != NULL);
        /* Relational/logical ops always yield int, but still widen mixed
         * int/float operands so both sides are float before the compare. */
        if (isRel) {
            if (lhs.isFloat || rhs.isFloat ||
                lhs.kind == OPND_CONST_FLOAT || rhs.kind == OPND_CONST_FLOAT) {
                lhs = ir_ensure_float(lhs, out);
                rhs = ir_ensure_float(rhs, out);
            }
            isF = 0;
        } else if (isF) {
            lhs = ir_ensure_float(lhs, out);
            rhs = ir_ensure_float(rhs, out);
        }
        Operand t   = (Operand){ .kind = OPND_TEMP, .isFloat = isF,
                                 .data.tempId = nextTemp++ };
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
    case ND_NUM_INT: {
        Operand src = (Operand){ .kind = OPND_CONST_INT, .data.intVal = atoi(expr->text) };
        if (dest.isFloat)
            src = ir_ensure_float(src, out);
        ir_emit_instr(out, IR_ASSIGN, dest, src, (Operand){.kind = OPND_NONE});
        return dest;
    }
    case ND_NUM_FLOAT:
        ir_emit_instr(out, IR_ASSIGN, dest, (Operand){ .kind = OPND_CONST_FLOAT, .isFloat = 1, .data.floatVal = (float)atof(expr->text) }, (Operand){.kind = OPND_NONE});
        return dest;
    case ND_ID: {
        Operand src = ir_mk_var(expr);
        if (dest.isFloat)
            src = ir_ensure_float(src, out);
        ir_emit_instr(out, IR_ASSIGN, dest, src, (Operand){.kind = OPND_NONE});
        return dest;
    }

    case ND_ARRAY_ACCESS: {
        Operand idx  = ir_emit_expr(expr->children[0], out);
        Operand base = ir_mk_var(expr);
        if (dest.isFloat && expr->dataType != T_FLOAT) {
            /* Load as int then widen (array element is int, dest is float). */
            Operand tmp = (Operand){ .kind = OPND_TEMP, .isFloat = 0, .data.tempId = nextTemp++ };
            ir_emit_instr(out, IR_LOAD_ARR, tmp, base, idx);
            Operand widened = ir_ensure_float(tmp, out);
            ir_emit_instr(out, IR_ASSIGN, dest, widened, (Operand){.kind = OPND_NONE});
        } else {
            ir_emit_instr(out, IR_LOAD_ARR, dest, base, idx);
        }
        return dest;
    }

    case ND_UNARY: {
        Operand v = ir_emit_expr(expr->children[0], out);
        if (dest.isFloat && op_key(expr->text) != KEY_NOT)
            v = ir_ensure_float(v, out);
        ir_emit_instr(out, op_key(expr->text) == KEY_NOT ? IR_NOT : IR_NEG, dest, v, (Operand){.kind = OPND_NONE});
        return dest;
    }

    case ND_BINOP: {
        unsigned short key = op_key(expr->text);
        if (key == KEY_AND || key == KEY_OR)
            return ir_emit_short_circuit_into(expr, out, dest);
        Operand lhs = ir_emit_expr(expr->children[0], out);
        Operand rhs = ir_emit_expr(expr->children[1], out);
        int isRel = (strchr("=!&|<>", expr->text[0]) != NULL);
        // Ensure dest inherits float-ness when the optimiser dropped dataType
        if (!dest.isFloat && !isRel && (lhs.isFloat || rhs.isFloat
                || lhs.kind == OPND_CONST_FLOAT || rhs.kind == OPND_CONST_FLOAT))
            dest.isFloat = 1;
        if (dest.isFloat || (isRel && (lhs.isFloat || rhs.isFloat
                || lhs.kind == OPND_CONST_FLOAT || rhs.kind == OPND_CONST_FLOAT))) {
            lhs = ir_ensure_float(lhs, out);
            rhs = ir_ensure_float(rhs, out);
        }
        ir_emit_instr(out, ir_binop_to_irop(expr->text), dest, lhs, rhs);
        return dest;
    }

    case ND_CALL: {
        int n = expr->nchildren;
        Operand *args = n > 0 ? malloc((size_t)n * sizeof(Operand)) : NULL;
        for (int i = 0; i < n; i++)
            args[i] = ir_emit_expr(expr->children[i], out);
        for (int i = 0; i < n; i++)
            ir_emit_instr(out, IR_PARAM, (Operand){.kind = OPND_NONE}, args[i], (Operand){.kind = OPND_NONE});
        free(args);
        // unlike ir_emit_call, the CALL result is written straight into dest
        if (dest.isFloat && expr->dataType != T_FLOAT) {
            Operand tmp = (Operand){ .kind = OPND_TEMP, .isFloat = 0, .data.tempId = nextTemp++ };
            ir_emit_instr(out, IR_CALL, tmp, (Operand){ .kind = OPND_FUNC, .data.funcName = expr->text }, (Operand){ .kind = OPND_CONST_INT, .data.intVal = expr->nchildren });
            Operand widened = ir_ensure_float(tmp, out);
            ir_emit_instr(out, IR_ASSIGN, dest, widened, (Operand){.kind = OPND_NONE});
        } else {
            ir_emit_instr(out, IR_CALL, dest, (Operand){ .kind = OPND_FUNC, .data.funcName = expr->text }, (Operand){ .kind = OPND_CONST_INT, .data.intVal = expr->nchildren });
        }
        return dest;
    }

    case ND_ASSIGN: {
        // "x = (y = z)": inner assignment computes its own target, then the
        // resulting value is additionally copied into dest for this context
        Operand inner = ir_emit_assign(expr, out);
        if (dest.isFloat)
            inner = ir_ensure_float(inner, out);
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
            ir_emit_expr_into(stmt->children[0], out, ir_mk_var(stmt));
        } else {
            // array initializer list: one STORE_ARR per element, index = position
            for (int i = 0; i < stmt->nchildren; i++) {
                Operand v = ir_emit_expr(stmt->children[i], out);
                ir_emit_instr(out, IR_STORE_ARR, ir_mk_var(stmt), (Operand){ .kind = OPND_CONST_INT, .data.intVal = i }, v);
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
        if (g_func_returns_float)
            v = ir_ensure_float(v, out);
        ir_emit_instr(out, IR_RETURN, (Operand){.kind = OPND_NONE}, v, (Operand){.kind = OPND_NONE});
        break;
    }

    default: break; // ND_FUNC_DECL/ND_PARAM never appear as statements; ND_ERROR ignored
    }
}

/* =========================================================================
 * Function compilation
 * ========================================================================= */

/* decl->text is "returnType funcName"; true when the return type is float. */
static int ir_func_returns_float(const ASTNode *decl) {
    if (!decl || !decl->text) return 0;
    return strncmp(decl->text, "float", 5) == 0 &&
           (decl->text[5] == ' ' || decl->text[5] == '\0');
}

/* Fall-through guard: emit return 0 / 0.0 so control cannot run into the next
 * function in .text. DCE drops it when every path already has a terminator. */
static void ir_emit_implicit_return(IRFunction *f, const ASTNode *decl) {
    Operand zero = ir_func_returns_float(decl)
        ? (Operand){ .kind = OPND_CONST_FLOAT, .isFloat = 1, .data.floatVal = 0.0f }
        : (Operand){ .kind = OPND_CONST_INT,   .data.intVal = 0 };
    ir_emit_instr(f, IR_RETURN, (Operand){ .kind = OPND_NONE }, zero,
                  (Operand){ .kind = OPND_NONE });
}

static IRFunction *ir_build_function(ASTNode *decl,Arena *arena) {
    // decl->text is "returnType funcName"; the name is everything after the last space
    const char *space = strrchr(decl->text, ' ');
    const char *name  = space ? space + 1 : decl->text;

    IRFunction *f = calloc(1, sizeof(IRFunction));
    f->name          = strdup(name);
    f->labelBase     = nextLabel; // labels for this function start where the last one left off
    f->curBlockStart = 0;
    currentLoopDepth = 0;
    g_func_returns_float = ir_func_returns_float(decl);

    int paramCount = decl->nchildren - 1;
    f->paramCount  = paramCount;
    f->params      = paramCount > 0 ? malloc((size_t)paramCount * sizeof(Operand)) : NULL;
    for (int p = 0; p < paramCount; p++)
        f->params[p] = ir_mk_var(decl->children[p]);

    ASTNode *body = decl->children[decl->nchildren - 1];
    ir_emit_stmt(body, f);
    ir_emit_implicit_return(f, decl);

    ir_resolve_cfg(f);

    gl_lower_globals(f,arena);

    svn_optimize(f);

    // Single VarMap shared by every cp_optimize/dce_optimize call
    VarMap *sharedVarMap = varmap_create();

    dce_optimize(f, sharedVarMap, arena);

    int changed;
    do {
        changed  = cp_optimize(f, sharedVarMap, arena);
        changed |= dce_optimize(f, sharedVarMap, arena);
    } while (changed);

    changed  = licm_optimize(f,arena);
    changed |= sr_optimize(f,arena);

    if (changed) {
        do {
            changed  = cp_optimize(f, sharedVarMap, arena);
            changed |= dce_optimize(f, sharedVarMap, arena);
        } while (changed);
    }

    varmap_destroy(sharedVarMap);

    return f;
}


static void ir_program_append(IRProgram *prog, IRFunction *f) {
    // standard doubling growth
    if (prog->count == prog->capacity) {
        prog->capacity = prog->capacity ? prog->capacity * 2 : 8;
        prog->functions = realloc(prog->functions,
                                  (size_t)prog->capacity * sizeof(IRFunction *));
    }
    prog->functions[prog->count++] = f;
}
int ir_alloc_temp_id(void){
    return nextTemp++;
}


IRProgram *ir_generate(ASTNode *program) {
    nextTemp  = 0;
    nextLabel = 0;

    IRProgram *prog = calloc(1, sizeof(IRProgram));
    Arena *arena = arena_create(0);

    ir_register_globals(prog, program);

    // compile every function body
    for (int i = 0; i < program->nchildren; i++) {
        ASTNode *decl = program->children[i];
        if (decl->kind == ND_FUNC_DECL)
            ir_program_append(prog, ir_build_function(decl,arena));
    }
    
    arena_destroy(arena);
    return prog;
}

/* ir_defines_dst / ir_is_commutative / ir_operand_is_storage /
 * ir_is_same_operand / ir_is_comparison / ir_is_binary_op → ir_op_info.c */

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

    // guard against a zero-size allocation when the function has no edges at all
    pl.predData = arena_alloc(arena, (size_t)(total > 0 ? total : 1) * sizeof(int));

    ir_fill_pred_data(f, n, pl.predStart, pl.predData, arena);

    return pl;
}