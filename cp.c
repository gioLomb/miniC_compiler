#include <stdlib.h>
#include <string.h>
#include "cp.h"
#include "arena.h"
#include "varmap.h"
#include "constmap.h"


static inline int is_binary_op(IROp op) {
    switch (op) {
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_LT:  case IR_LE:  case IR_GT:  case IR_GE:  case IR_EQ: case IR_NE:
        return 1;
    default: return 0;
    }
}

static int cp_is_comparison_op(IROp op) {
    switch (op) {
    case IR_LT: case IR_LE: case IR_GT: case IR_GE: case IR_EQ: case IR_NE:
        return 1;
    default: return 0;
    }
}




/**
 * @brief Constant-fold a binary integer operation.
 *
 * Guards division and modulo by zero: returning 0 (not folded) defers
 * execution to runtime, preserving whatever behaviour the target platform
 * defines for integer division by zero.  Folding to an arbitrary value
 * would silently change program semantics.
 */
static inline int fold_binary_int(IROp op, int a, int b, int *res) {
    switch (op) {
    case IR_ADD: *res = a + b;                        return 1;
    case IR_SUB: *res = a - b;                        return 1;
    case IR_MUL: *res = a * b;                        return 1;
    case IR_DIV: if (!b) return 0; *res = a / b;      return 1; // b==0: defer to runtime
    case IR_MOD: if (!b) return 0; *res = a % b;      return 1; // b==0: defer to runtime
    case IR_LT:  *res = (a <  b);                     return 1;
    case IR_LE:  *res = (a <= b);                     return 1;
    case IR_GT:  *res = (a >  b);                     return 1;
    case IR_GE:  *res = (a >= b);                     return 1;
    case IR_EQ:  *res = (a == b);                     return 1;
    case IR_NE:  *res = (a != b);                     return 1;
    default:                                           return 0; // unhandled opcode
    }
}

/**
 * @brief Constant-fold a binary float operation.
 *
 * Float division by zero is guarded the same way as integer division.
 * Comparison results are stored as float (0.0 or 1.0) and the caller is
 * responsible for converting them to int when is_comparison_op() is true.
 */
static inline int fold_binary_float(IROp op, float a, float b, float *res) {
    switch (op) {
    case IR_ADD: *res = a + b;                                 return 1;
    case IR_SUB: *res = a - b;                                 return 1;
    case IR_MUL: *res = a * b;                                 return 1;
    case IR_DIV: if (b == 0.0f) return 0; *res = a / b;       return 1; // b==0: defer to runtime
    case IR_LT:  *res = (float)(a <  b);                       return 1;
    case IR_LE:  *res = (float)(a <= b);                       return 1;
    case IR_GT:  *res = (float)(a >  b);                       return 1;
    case IR_GE:  *res = (float)(a >= b);                       return 1;
    case IR_EQ:  *res = (float)(a == b);                       return 1;
    case IR_NE:  *res = (float)(a != b);                       return 1;
    default:                                                    return 0;
    }
}


/**
 * @brief Update @p map in-place according to the effect of instruction @p in.
 *
 * Only instructions that define a variable/temporary are interesting:
 *   - IR_ASSIGN from a known constant → propagate the constant.
 *   - Binary/unary ops on two known constants → fold to constant.
 *   - Any other defining instruction → set dst to LAT_CONFLICT
 *     (value unknown at compile time).
 *
 * Non-defining opcodes (IR_PARAM, IR_GOTO, IR_IF_FALSE, IR_STORE_ARR,
 * IR_RETURN, IR_LABEL) have no effect on the map.
 */

// cp_transfer: drop 'i' param, resolve ids via varmap_operand_id directly
static void cp_transfer(const IRInstr *in, ConstMap *map, VarMap *vm) {
    if (!ir_defines_dst(in->op))
        return;

    int dstId = varmap_operand_id(vm, in->dst);
    if (dstId < 0 || dstId >= map->size) return;

    LatVal result = lat_conflict();
    int src1Id = varmap_operand_id(vm, in->src1);

    if (in->op == IR_ASSIGN) {
        result = lat_get_value_by_id(map, in->src1, src1Id);
    } else if (is_binary_op(in->op)) {
        int src2Id = varmap_operand_id(vm, in->src2);
        LatVal lhs = lat_get_value_by_id(map, in->src1, src1Id);
        LatVal rhs = lat_get_value_by_id(map, in->src2, src2Id);
        if (lhs.state == LAT_CONST && rhs.state == LAT_CONST) {
            if (!lhs.isFloat && !rhs.isFloat) {
                int r;
                if (fold_binary_int(in->op, lhs.val.ival, rhs.val.ival, &r))
                    result = lat_set_const_int(r);
            } else if (lhs.isFloat && rhs.isFloat) {
                float r;
                if (fold_binary_float(in->op, lhs.val.fval, rhs.val.fval, &r))
                    result = cp_is_comparison_op(in->op)
                             ? lat_set_const_int((int)r)
                             : lat_set_const_float(r);
            }
        }
    } else if (in->op == IR_NEG || in->op == IR_NOT) {
        result = lat_fold_unary(in->op, lat_get_value_by_id(map, in->src1, src1Id));
    }

    map->vals[dstId] = result;
}


/**
 * @brief Compute in[b] and out[b] for every block to a fixed point.
 *
 * in[b]  = meet of out[p] for all predecessors p of b.
 * out[b] = transfer(in[b], instructions of b).
 *
 * The iteration is purely forward; blocks are processed in index order,
 * which is already roughly topological for reducible CFGs.  The outer
 * while loop repeats until no Out[] set changes — guaranteed to terminate
 * because lattice values only descend (UNKNOWN→CONST→CONFLICT).
 *
 * Predecessor lookup uses the precomputed @p preds CSR list (ir.h) instead
 * of scanning every block's succ[] on every fixed-point iteration — the
 * scan-based approach cost O(nBlocks) per block per iteration, i.e.
 * O(nBlocks^2) per pass; the CSR list turns it into O(nBlocks + edges)
 * per pass, with the list itself built once by the caller.
 *
 * @param f       Function being analysed.
 * @param in      Per-block input ConstMaps (one entry per block, arena-allocated).
 * @param out     Per-block output ConstMaps.
 * @param tmp     Scratch ConstMap (arena-allocated, same size as in[b]).
 * @param vm      VarMap for operand-to-id translation.
 * @param preds   Precomputed predecessor list for @p f (see ir_build_pred_list).
 */
static void cp_run_forward_dataflow(IRFunction *f, ConstMap *in, ConstMap *out,
                                  ConstMap *tmp, VarMap *vm,
                                  const PredList *preds) {
    int nBlocks = f->blockCount;
    int changed = 1;

    while (changed) {
        changed = 0;
        for (int b = 0; b < nBlocks; b++) {

            int start = preds->predStart[b];
            for (int i = 0; i < preds->predCount[b]; i++) {
                int p = preds->predData[start + i];
                if (i == 0) const_map_copy(&in[b], &out[p]); // first predecessor: copy
                else        const_map_meet(&in[b], &out[p]); // subsequent: meet (join)
            }
            // block with no predecessors keeps in[b] = all UNKNOWN (initial value)

            // compute new out[b] = transfer(in[b]) into tmp
            const_map_copy(tmp, &in[b]);

            for (int i = f->blocks[b].bb.range.start; i < f->blocks[b].bb.range.end; i++)
                cp_transfer(&f->instrs[i], tmp, vm);

            // if out[b] changed, record it and keep iterating
            if (!const_map_equal(&out[b], tmp)) {
                const_map_copy(&out[b], tmp);
                changed = 1;
            }
        }
    }
}


/**
 * @brief Fold a binary instruction in-place when both operands are constants.
 *
 * Handles int/float promotion: if either operand is float the operation
 * is performed in float arithmetic.  Comparison operators always yield
 * an int result (0 or 1) regardless of operand type.
 *
 * The arithmetic kernel (fold_binary_int / fold_binary_float) lives in
 * constmap.c; this function only handles the int→float promotion logic
 * and rewrites the instruction on success.
 *
 * On success rewrites @p in to IR_ASSIGN with the folded constant as src1.
 * Division/modulo by zero is left unfolded (returns 0) so the runtime
 * can raise the appropriate exception.
 *
 * @return 1 if folding succeeded, 0 otherwise.
 */
static int cp_fold_binary(IRInstr *in, const Operand *ns1, const Operand *ns2) {
    int floatOp = (ns1->kind == OPND_CONST_FLOAT || ns2->kind == OPND_CONST_FLOAT);
    IROp origOp = in->op;

    if (floatOp) {
        // promote int operand to float before folding
        float a = (ns1->kind == OPND_CONST_INT) ? (float)ns1->data.intVal
                                                 : ns1->data.floatVal;
        float b = (ns2->kind == OPND_CONST_INT) ? (float)ns2->data.intVal
                                                 : ns2->data.floatVal;
        float result;
        if (!fold_binary_float(origOp, a, b, &result)) return 0;

        in->op   = IR_ASSIGN;
        in->src2 = (Operand){.kind = OPND_NONE};
        // comparisons produce int 0/1 even when operands are float
        if (cp_is_comparison_op(origOp)) {
            in->src1 = (Operand){ .kind = OPND_CONST_INT, .isFloat = 0,
                                  .data.intVal = (int)result };
        } else {
            in->src1 = (Operand){ .kind = OPND_CONST_FLOAT, .isFloat = 1,
                                  .data.floatVal = result };
        }

    } else {
        int a = ns1->data.intVal;
        int b = ns2->data.intVal;
        int result;
        if (!fold_binary_int(origOp, a, b, &result)) return 0; // e.g. div/mod by 0

        in->op   = IR_ASSIGN;
        in->src1 = (Operand){ .kind = OPND_CONST_INT, .isFloat = 0,
                              .data.intVal = result };
        in->src2 = (Operand){.kind = OPND_NONE};
    }
    return 1;
}

/**
 * @brief Return 1 if the jump at @p idx unconditionally targets the next instr.
 *
 * A GOTO or IF_FALSE whose label resolves to the immediately following
 * instruction is a no-op jump: it can be deleted, and the corresponding
 * CFG edge removed.
 */
static int cp_is_redundant_jump(IRFunction *f, int idx) {
    IRInstr *in = &f->instrs[idx];
    if (in->op != IR_GOTO && in->op != IR_IF_FALSE) return 0;
    if (idx + 1 >= f->count) return 0;

    // blocks partition [0, f->count) contiguously with no gaps, so idx+1
    // (already known valid) belongs to exactly one block: no need to find
    // which one, just inspect the instruction directly
    IRInstr *next = &f->instrs[idx + 1];
    return next->op == IR_LABEL && next->dst.data.labelId == in->dst.data.labelId;
}

/**
 * @brief Build a labelId -> instruction-index lookup for @p f.
 *
 * Every label id is globally unique (see nextLabel counter in ir.c), so
 * this map has no collisions. Sized to f->count: a function can never
 * contain more labels than instructions, so any idx computed from a label
 * belonging to this function is guaranteed < f->count.
 *
 * @param outCap Set to the map's element count (== f->count, or 1 if empty).
 * @return       Arena-allocated map; entries default to -1 ("no label with
 *               this id found in this function").
 */
static int *build_label_to_instr(const IRFunction *f, Arena *arena, int *outCap) {
    int cap = f->count > 0 ? f->count : 1;
    int *map = arena_alloc(arena, (size_t)cap * sizeof(int));
    memset(map, -1, (size_t)cap * sizeof(int));

    const IRInstr *instrs = f->instrs;
    int labelBase = f->labelBase;

    for (int i = 0; i < f->count; i++) {
        if (instrs[i].op == IR_LABEL) {
            int idx = instrs[i].dst.data.labelId - labelBase;
            if (idx >= 0 && idx < cap && map[idx] < 0)
                map[idx] = i;
        }
    }

    *outCap = cap;
    return map;
}


static int label_map_index(const IRFunction *f, int labelId, int mapCap) {
    int idx = labelId - f->labelBase;
    return (idx >= 0 && idx < mapCap) ? idx : -1;
}

/**
 * @brief Mark every IR_LABEL that is the target of a GOTO / IF_FALSE.
 *
 * If the same labelId appears more than once, labelToInstr keeps only the
 * last occurrence; any referenced id is then propagated to all copies so
 * that no live label is treated as dead.
 */
static void collect_referenced_labels(const IRFunction *f,
                                      const int *labelToInstr, int mapCap,
                                      char *referenced) {
    /* Pass 1: mark the canonical (last) copy of each branch target. */
    for (int i = 0; i < f->count; i++) {
        const IRInstr *in = &f->instrs[i];
        if (in->op != IR_GOTO && in->op != IR_IF_FALSE)
            continue;

        int idx = label_map_index(f, in->dst.data.labelId, mapCap);
        if (idx < 0)
            continue;

        int at = labelToInstr[idx];
        if (at >= 0)
            referenced[at] = 1;
    }

    /* Pass 2: if any copy of a labelId is live, mark every copy live. */
    for (int i = 0; i < f->count; i++) {
        if (f->instrs[i].op != IR_LABEL)
            continue;

        int idx = label_map_index(f, f->instrs[i].dst.data.labelId, mapCap);
        if (idx < 0)
            continue;

        int at = labelToInstr[idx];
        if (at >= 0 && referenced[at])
            referenced[i] = 1;
    }
}

static int mark_unreferenced_labels(const IRFunction *f, const char *referenced, char *eliminate) {
    int count = 0;
    for (int i = 0; i < f->count; i++) {
        if (f->instrs[i].op == IR_LABEL && !referenced[i]) {
            eliminate[i] = 1;
            count++;
        }
    }
    return count;
}

/**
 * @brief Mark IR_LABEL instructions that no jump in the function references.
 *
 * @param f      Function to scan.
 * @param eliminate Boolean array (one entry per instruction); entries for
 *                  orphan labels are set to 1.
 * @param arena  Scratch arena for the referenced[] flags and labelToInstr map
 *               (caller-owned, reset by cp_optimize() at the start of each call).
 * @return       Number of labels newly marked for elimination.
 */
static int cp_mark_orphan_labels(IRFunction *f, char *eliminate, Arena *arena) {
    char *referenced = arena_alloc(arena, (size_t)f->count);
    memset(referenced, 0, (size_t)f->count);

    int mapCap;
    int *labelToInstr = build_label_to_instr(f, arena, &mapCap);

    collect_referenced_labels(f, labelToInstr, mapCap, referenced);
    return mark_unreferenced_labels(f, referenced, eliminate);
}

/**
 * @brief Eliminate a GOTO/IF_FALSE that unconditionally jumps to the next instruction.
 *
 * The jump contributes no reachable CFG edge (fall-through already goes there),
 * so it is marked for deletion and the corresponding predCount is decremented.
 * Caller guarantees cp_is_redundant_jump(f, i) already returned true.
 *
 * @return Always 1 (the instruction is always eliminated).
 */
static int try_eliminate_redundant_jump(IRFunction *f, int b, int i, char *eliminate) {
    IRInstr *in = &f->instrs[i];

    if (in->op == IR_GOTO) {
        // GOTO's target becomes an implicit fallthrough (jump physically
        // adjacent to its label): succ[0] already points there and must
        // stay untouched — the edge survives, only the explicit jump
        // instruction is removed. No predCount change (edge count unchanged).
    } else {
        // IF_FALSE: succ[0] (fallthrough) and succ[1] (taken) point to the
        // SAME block here — that's what makes the jump redundant. Collapse
        // the now-duplicate taken edge into the fallthrough.
        int s = f->blocks[b].bb.succ[1];
        if (s >= 0) f->blocks[s].predCount--;
        f->blocks[b].bb.succ[1] = -1;
    }

    eliminate[i] = 1;
    return 1;
}
/**
 * @brief Fold an IR_IF_FALSE whose condition is (now) known at compile time,
 *        pruning the corresponding CFG edge; otherwise substitute the
 *        condition operand if it simplified without becoming constant.
 *
 * @param f    Function being rewritten (succ[]/predCount updated in place).
 * @param b    Index of the block containing the instruction.
 * @param i    Instruction index (must be IR_IF_FALSE).
 * @param live Current local constant-propagation state (read-only here).
 * @param vm   VarMap for operand resolution.
 * @return     1 if the instruction was modified, 0 otherwise.
 */
static int try_fold_if_false(IRFunction *f, int b, int i, ConstMap *live, int src1Id) {
    IRInstr *in  = &f->instrs[i];
    Operand  cond = const_map_try_fold_by_id(in->src1, live, src1Id);

    if (cond.kind != OPND_CONST_INT && cond.kind != OPND_CONST_FLOAT) {
        // not fully constant: keep only if the substitution actually changed something
        if (ir_is_same_operand(&cond, &in->src1)) return 0;
        in->src1 = cond;
        return 1;
    }

    int isZero = (cond.kind == OPND_CONST_INT) ? (cond.data.intVal   == 0)
                                                : (cond.data.floatVal == 0.0f);
    int taken = isZero; // IF_FALSE jumps when condition is false (0)
    int s0 = f->blocks[b].bb.succ[0]; // fall-through
    int s1 = f->blocks[b].bb.succ[1]; // taken (jump target)

    if (taken) {
        // condition always false -> branch always taken: becomes unconditional GOTO
        in->op   = IR_GOTO;
        in->src1 = in->src2 = (Operand){ .kind = OPND_NONE };
        f->blocks[b].bb.succ[0] = s1;
        f->blocks[b].bb.succ[1] = -1;
        if (s0 >= 0) f->blocks[s0].predCount--;
    } else {
        // condition always true -> branch never taken: caller marks eliminate[i]
        f->blocks[b].bb.succ[1] = -1;
        if (s1 >= 0) f->blocks[s1].predCount--;
    }
    return 1;
}

/**
 * @brief Substitute known-constant operands and fold a binary op if both
 *        source operands became constant.
 *
 * Does NOT touch control-flow opcodes (IR_IF_FALSE handled separately by
 * the caller; IR_GOTO/IR_LABEL/IR_RETURN carry no foldable src1/src2 pair).
 *
 * @return 1 if the instruction was modified (substitution and/or fold), 0 otherwise.
 */
static int try_fold_generic_instr(IRInstr *in, ConstMap *live, int src1Id, int src2Id) {
    int modified = 0;

    Operand ns1 = const_map_try_fold_by_id(in->src1, live, src1Id);
    Operand ns2 = const_map_try_fold_by_id(in->src2, live, src2Id);
    if (!ir_is_same_operand(&ns1, &in->src1)) { in->src1 = ns1; modified = 1; }
    if (!ir_is_same_operand(&ns2, &in->src2)) { in->src2 = ns2; modified = 1; }

    // only binary arithmetic/relational opcodes are foldable here
    int isBinaryFoldable =
        in->op == IR_ADD || in->op == IR_SUB || in->op == IR_MUL ||
        in->op == IR_DIV || in->op == IR_MOD ||
        in->op == IR_LT  || in->op == IR_LE  || in->op == IR_GT  ||
        in->op == IR_GE  || in->op == IR_EQ  || in->op == IR_NE;

    if (isBinaryFoldable &&
        (ns1.kind == OPND_CONST_INT || ns1.kind == OPND_CONST_FLOAT) &&
        (ns2.kind == OPND_CONST_INT || ns2.kind == OPND_CONST_FLOAT)) {
        modified |= cp_fold_binary(in, &ns1, &ns2);
    }

    return modified;
}


/**
 * @brief Rewrite one basic block using the constant information in @p in[b].
 *
 * Performs three kinds of transformations in a single forward scan:
 *
 *  1. Jump-to-next elimination: GOTO/IF_FALSE that jumps to the very next
 *     instruction is deleted; the redundant CFG edge is removed.
 *
 *  2. IF_FALSE folding: if the condition is a known constant at this point,
 *     replace with IR_GOTO (condition false → branch taken) or delete
 *     (condition true → fall-through); update succ[]/predCount.
 *
 *  3. Constant substitution and binary folding: replace variable uses with
 *     their known constant value; if both operands of a binary instruction
 *     become constants, fold the instruction to IR_ASSIGN of the result.
 *
 * A local copy of the ConstMap (`live`) is maintained and advanced through
 * the block so that constants defined early in the block are available
 * for instructions later in the same block (within-block propagation).
 *
 * @return 1 if any instruction in the block was modified or eliminated.
 */
static int cp_rewrite_block(IRFunction *f, int b, ConstMap *inMap,
                             char *eliminate, VarMap *vm, Arena *arena) {
    ConstMap live;
    const_map_init(&live, varmap_count(vm), arena);
    const_map_copy(&live, &inMap[b]);

    int modified = 0;
    for (int i = f->blocks[b].bb.range.start; i < f->blocks[b].bb.range.end; i++) {
        IRInstr *in = &f->instrs[i];

        if ((in->op == IR_GOTO || in->op == IR_IF_FALSE) && cp_is_redundant_jump(f, i)) {
            modified |= try_eliminate_redundant_jump(f, b, i, eliminate);
        } else if (in->op == IR_IF_FALSE) {
            modified |= try_fold_if_false(f, b, i, &live, varmap_operand_id(vm, in->src1));
        } else {
            modified |= try_fold_generic_instr(in, &live,
                            varmap_operand_id(vm, in->src1),
                            varmap_operand_id(vm, in->src2));
        }
        cp_transfer(in, &live, vm);
    }
    return modified;
}



/**
 * @brief Register every operand of @p f into @p vm (get-or-create).
 *
 * Idempotent: when @p vm was already populated by a previous call (shared
 * across the CP/DCE loop), this only performs cheap lookups (hits) for
 * known operands; only genuinely new operands (e.g. SR temps introduced
 * after LICM+SR) trigger an actual insert.
 */
static void cp_register_operands(VarMap *vm, IRFunction *f) {
    for (int i = 0; i < f->count; i++) {
        IRInstr *in = &f->instrs[i];
        varmap_operand_id(vm, in->dst);
        varmap_operand_id(vm, in->src1);
        varmap_operand_id(vm, in->src2);
    }
}

int cp_optimize(IRFunction *f, VarMap *vm, Arena *arenaScratch) {
    if (!f || f->blockCount == 0 || f->count == 0) return 0;

    int nBlocks = f->blockCount;
    arena_reset(arenaScratch);

    // Registers any new operand and refreshes the per-instruction id cache
    // only if f changed structurally since the caller's last sync.
    cp_register_operands(vm, f);
    int numVars = varmap_count(vm);
    PredList preds = ir_build_pred_list(f, arenaScratch);

    ConstMap *in  = arena_alloc(arenaScratch, (size_t)nBlocks * sizeof(ConstMap));
    ConstMap *out = arena_alloc(arenaScratch, (size_t)nBlocks * sizeof(ConstMap));
    ConstMap  tmp;
    const_map_init(&tmp, numVars, arenaScratch);
    for (int b = 0; b < nBlocks; b++) {
        const_map_init(&in[b],  numVars, arenaScratch);
        const_map_init(&out[b], numVars, arenaScratch);
    }

    cp_run_forward_dataflow(f, in, out, &tmp, vm, &preds);

    int modified = 0;
    char *eliminate = arena_alloc(arenaScratch, (size_t)f->count * sizeof(char));
    memset(eliminate, 0, (size_t)f->count * sizeof(char));

    for (int b = 0; b < nBlocks; b++)
        modified |= cp_rewrite_block(f, b, in, eliminate, vm, arenaScratch);

    modified |= (cp_mark_orphan_labels(f, eliminate, arenaScratch) > 0);
    if (modified) modified = ir_sweep(f, eliminate, nBlocks);

    return modified;
}