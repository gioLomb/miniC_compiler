/**
 * @file cp.c
 * @brief Constant Propagation + CFG Pruning on the linear IR.
 *
 * Algorithm overview (forward dataflow, iterative to fixed point):
 * ----------------------------------------------------------------
 * Each basic block b has an in[b] and out[b] ConstMap.  A ConstMap
 * maps every variable/temporary id (from VarMap) to a lattice value:
 *
 *   LAT_UNKNOWN  (⊤) — no definition reaches this point yet
 *   LAT_CONST       — exactly one constant value reaches on all paths
 *   LAT_CONFLICT (⊥) — two or more different values reach (not a constant)
 *
 * Lattice meet (join at confluence points):
 *   UNKNOWN  meet X       = X
 *   X        meet UNKNOWN = X
 *   CONFLICT meet X       = CONFLICT
 *   CONST(a) meet CONST(b) = CONST(a)  if a==b, else CONFLICT
 *
 * Forward transfer function for a block: start from in[b], simulate
 * each instruction updating the map.  IR_ASSIGN of a known constant
 * propagates it; any other definition sets the variable to CONFLICT.
 *
 * After convergence, rewrite pass:
 *   - Replace variable uses with their known constant value inline.
 *   - Fold binary/unary operations on newly constant operands.
 *   - IR_IF_FALSE with constant condition → IR_GOTO (taken) or deleted
 *     (not taken); update succ[]/predCount in the CFG (CFG pruning).
 *   - Unconditional jumps to the immediately following instruction
 *     (jump-to-next) → deleted (CFG pruning).
 *   - Orphan IR_LABEL nodes (no jump references them) → deleted.
 *
 * The sweep step compacts the instruction array and adjusts block
 * [start, end) ranges to match.
 *
 * Returns 1 if any instruction was rewritten or deleted (caller
 * re-runs DCE and CP until fixed point).
 *
 * Internal structure
 * ------------------
 *  cp_build_varmap          — assign compact int ids to all operands
 *  cp_run_forward_dataflow  — forward fixed-point loop filling in[]/out[]
 *  cp_rewrite_block         — rewrite + CFG prune one block using in[b]
 *  ir_sweep                 — compact instruction array, update block ranges
 *  cp_optimize              — public entry point, orchestrates the above
 *
 * Helper delegation:
 *  cp_transfer              — in-place ConstMap update for one instruction
 *  cp_fold_binary           — int/float binary constant fold with promotion
 *  cp_is_redundant_jump     — detect GOTO/IF_FALSE → immediately next instr
 *  cp_mark_orphan_labels    — find IR_LABEL nodes with no referencing jump
 */

#include <stdlib.h>
#include <string.h>
#include "cp.h"
#include "arena.h"
#include "varmap.h"
#include "constmap.h"

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/**
 * @brief Structural equality for two Operand values.
 *
 * Used by the rewrite pass to detect when substituting a constant into
 * an operand actually changed it (avoids spurious modified=1 signals).
 */
static inline int operand_equal(const Operand *a, const Operand *b) {
    if (a->kind != b->kind) return 0;
    if (a->kind == OPND_CONST_INT)   return a->data.intVal   == b->data.intVal;
    if (a->kind == OPND_CONST_FLOAT) return a->data.floatVal == b->data.floatVal;
    if (a->kind == OPND_VAR)
        return a->data.varLevel  == b->data.varLevel &&
               a->data.varOffset == b->data.varOffset;
    if (a->kind == OPND_TEMP)  return a->data.tempId  == b->data.tempId;
    if (a->kind == OPND_LABEL) return a->data.labelId == b->data.labelId;
    if (a->kind == OPND_FUNC)  return strcmp(a->data.funcName, b->data.funcName) == 0;
    return 1; /* OPND_NONE */
}

/* =========================================================================
 * Transfer function
 * ========================================================================= */

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

 static void cp_transfer(const IRInstr *in, ConstMap *map, VarMap *vm) {
    /* Le istruzioni che non definiscono un risultato non alterano la mappa */
    if (!ir_defines_dst(in->op))
        return;

    int id = varmap_operand_id(vm, in->dst);
    if (id < 0 || id >= map->size) return;

    LatVal result = lat_conflict(); // default

    if (in->op == IR_ASSIGN) {
        result = lat_get_value_from_operand(map, in->src1, vm);
    } else if (is_binary_op(in->op)) {
        LatVal lhs = lat_get_value_from_operand(map, in->src1, vm);
        LatVal rhs = lat_get_value_from_operand(map, in->src2, vm);
        if (lhs.state == LAT_CONST && rhs.state == LAT_CONST) {
            if (!lhs.isFloat && !rhs.isFloat) {
                int r;
                if (fold_binary_int(in->op, lhs.val.ival, rhs.val.ival, &r))
                    result = lat_set_const_int(r);
            } else if (lhs.isFloat && rhs.isFloat) {
                float r;
                if (fold_binary_float(in->op, lhs.val.fval, rhs.val.fval, &r))
                    result = is_comparison_op(in->op)
                             ? lat_set_const_int((int)r)
                             : lat_set_const_float(r);
            }
        }
    } else if (in->op == IR_NEG || in->op == IR_NOT) {
        result = fold_unary(in->op, lat_get_value_from_operand(map, in->src1, vm));
    }

    map->vals[id] = result;
}

// static void cp_transfer(const IRInstr *in, ConstMap *map, VarMap *vm) {
//     int id = varmap_operand_id(vm, in->dst);
//     if (id < 0 || id >= map->size) return; // dst not a tracked storage location

//     LatVal result = lat_conflict(); // default: assume unknown/conflicting

//     if (in->op == IR_ASSIGN) {
//         // copy: propagate whatever lattice value src1 currently has
//         result = lat_get_value_from_operand(map, in->src1, vm);

//     } else if (is_binary_op(in->op)) {
//         LatVal lhs = lat_get_value_from_operand(map, in->src1, vm);
//         LatVal rhs = lat_get_value_from_operand(map, in->src2, vm);
//         // only fold when both operands are known constants
//         if (lhs.state == LAT_CONST && rhs.state == LAT_CONST) {
//             if (!lhs.isFloat && !rhs.isFloat) {
//                 int r;
//                 if (fold_binary_int(in->op, lhs.val.ival, rhs.val.ival, &r))
//                     result = lat_set_const_int(r);
//             } else if (lhs.isFloat && rhs.isFloat) {
//                 float r;
//                 if (fold_binary_float(in->op, lhs.val.fval, rhs.val.fval, &r))
//                     result = is_comparison_op(in->op)
//                              ? lat_set_const_int((int)r)   // comparison → int 0/1
//                              : lat_set_const_float(r);
//             }
//             // mixed int/float: leave as LAT_CONFLICT (no implicit promotion here)
//         }

//     } else if (in->op == IR_NEG || in->op == IR_NOT) {
//         result = fold_unary(in->op, lat_get_value_from_operand(map, in->src1, vm));
//     }

//     map->vals[id] = result;
// }

/* =========================================================================
 * Pass 1: build VarMap
 * ========================================================================= */

/**
 * @brief Scan all instructions and assign a compact int id to every operand.
 *
 * Must be called before any ConstMap operations so that every
 * variable/temporary that appears in the function has a valid id in [0, nextId).
 */
static void cp_build_varmap(IRFunction *f, VarMap *vm) {
    varmap_init(vm);
    for (int i = 0; i < f->count; i++) {
        IRInstr *in = &f->instrs[i];
        // register dst, src1, src2 — get-or-create semantics
        varmap_operand_id(vm, in->dst);
        varmap_operand_id(vm, in->src1);
        varmap_operand_id(vm, in->src2);
    }
}

/* =========================================================================
 * Pass 3: forward dataflow
 * ========================================================================= */

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
 * @param f       Function being analysed.
 * @param in      Per-block input ConstMaps (one entry per block, arena-allocated).
 * @param out     Per-block output ConstMaps.
 * @param tmp     Scratch ConstMap (arena-allocated, same size as in[b]).
 * @param numVars Total number of tracked variable ids (== vm->nextId).
 * @param vm      VarMap for operand-to-id translation.
 */
static void cp_run_forward_dataflow(IRFunction *f, ConstMap *in, ConstMap *out,
                                  ConstMap *tmp, int numVars, VarMap *vm) {
    (void)numVars;
    int nBlocks = f->blockCount;
    int changed = 1;

    while (changed) {
        changed = 0;
        for (int b = 0; b < nBlocks; b++) {

            // in[b] = meet of all predecessor out[p]
            int hasPred = 0;
            for (int p = 0; p < nBlocks; p++) {
                for (int k = 0; k < 2; k++) {
                    if (f->blocks[p].bb.succ[k] != b) continue;
                    if (!hasPred) {
                        const_map_copy(&in[b], &out[p]); // first predecessor: copy
                        hasPred = 1;
                    } else {
                        const_map_meet(&in[b], &out[p]); // subsequent: meet (join)
                    }
                }
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

/* =========================================================================
 * CFG pruning helpers
 * ========================================================================= */

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
        in->src2 = no_operand();
        // comparisons produce int 0/1 even when operands are float
        if (is_comparison_op(origOp)) {
            in->src1 = (Operand){ .kind = OPND_CONST_INT,
                                  .data.intVal = (int)result };
        } else {
            in->src1 = (Operand){ .kind = OPND_CONST_FLOAT,
                                  .data.floatVal = result };
        }

    } else {
        int a = ns1->data.intVal;
        int b = ns2->data.intVal;
        int result;
        if (!fold_binary_int(origOp, a, b, &result)) return 0; // e.g. div/mod by 0

        in->op   = IR_ASSIGN;
        in->src1 = (Operand){ .kind = OPND_CONST_INT,
                              .data.intVal = result };
        in->src2 = no_operand();
    }
    return 1;
}

/**
 * @brief Return 1 if the jump at @p idx unconditionally targets the next instr.
 *
 * A GOTO or IF_FALSE whose label resolves to the immediately following
 * instruction is a no-op jump: it can be deleted, and the corresponding
 * CFG edge removed.  We detect this by checking whether the instruction
 * at idx+1 is an IR_LABEL with the same labelId as the jump target.
 */
static int cp_is_redundant_jump(IRFunction *f, int idx) {
    if (idx + 1 >= f->count) return 0;
    IRInstr *in = &f->instrs[idx];
    if (in->op != IR_GOTO && in->op != IR_IF_FALSE) return 0;
    int labelId = in->dst.data.labelId;
    // scan blocks to confirm idx+1 is inside a block and starts with the label
    for (int b = 0; b < f->blockCount; b++) {
        int start = f->blocks[b].bb.range.start;
        int end   = f->blocks[b].bb.range.end;
        if (start <= idx + 1 && idx + 1 < end) {
            if (f->instrs[idx + 1].op == IR_LABEL &&
                f->instrs[idx + 1].dst.data.labelId == labelId) {
                return 1;
            }
        }
    }
    return 0;
}

/**
 * @brief Mark IR_LABEL instructions that no jump in the function references.
 *
 * An orphan label is safe to delete: it carries no semantic information
 * since no control-flow edge targets it.  This typically arises after
 * constant-folded IF_FALSE instructions are removed.
 *
 * @param f         Function to scan.
 * @param eliminate Boolean array (one entry per instruction); entries for
 *                  orphan labels are set to 1.
 * @return          Number of labels newly marked for elimination.
 */
static int cp_mark_orphan_labels(IRFunction *f, char *eliminate) {
    int count = 0;
    // referenced[j] = 1 if instr j is an IR_LABEL targeted by some jump
    char *referenced = calloc((size_t)f->count, 1);

    for (int i = 0; i < f->count; i++) {
        if (f->instrs[i].op != IR_GOTO && f->instrs[i].op != IR_IF_FALSE) continue;
        int labelId = f->instrs[i].dst.data.labelId;
        // find the IR_LABEL instruction with this id and mark it referenced
        for (int j = 0; j < f->count; j++) {
            if (f->instrs[j].op == IR_LABEL &&
                f->instrs[j].dst.data.labelId == labelId) {
                referenced[j] = 1;
                break;
            }
        }
    }

    for (int i = 0; i < f->count; i++) {
        if (f->instrs[i].op == IR_LABEL && !referenced[i]) {
            eliminate[i] = 1;
            count++;
        }
    }

    free(referenced);
    return count;
}

/* =========================================================================
 * Pass 4: rewrite + CFG pruning (one block)
 * ========================================================================= */

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
    int numVars  = vm->nextId;
    int modified = 0;

    // local copy of inMap[b]: tracks values as we advance through the block
    ConstMap live;
    const_map_init(&live, numVars, arena);
    const_map_copy(&live, &inMap[b]);

    for (int i = f->blocks[b].bb.range.start; i < f->blocks[b].bb.range.end; i++) {
        IRInstr *in = &f->instrs[i];

        /* ---- 1. Jump-to-next elimination ---- */
        if ((in->op == IR_GOTO || in->op == IR_IF_FALSE) && cp_is_redundant_jump(f, i)) {
            // the jumped-to block loses this predecessor
            int s = (in->op == IR_GOTO)
                    ? f->blocks[b].bb.succ[0]
                    : f->blocks[b].bb.succ[1]; // taken edge of IF_FALSE
            if (s >= 0) f->blocks[s].predCount--;

            if (in->op == IR_GOTO) {
                // GOTO deleted: only successor was succ[0], now none
                f->blocks[b].bb.succ[0] = f->blocks[b].bb.succ[1];
                f->blocks[b].bb.succ[1] = -1;
            } else {
                // IF_FALSE deleted: taken edge (succ[1]) removed; fall-through stays
                f->blocks[b].bb.succ[1] = -1;
            }
            eliminate[i] = 1;
            modified = 1;
            cp_transfer(in, &live, vm); // still advance the map
            continue;
        }

        /* ---- 2. IF_FALSE constant folding and CFG pruning ---- */
        if (in->op == IR_IF_FALSE) {
            Operand cond = const_map_try_fold(in->src1, &live, vm);
            if (cond.kind == OPND_CONST_INT || cond.kind == OPND_CONST_FLOAT) {
                int isZero = (cond.kind == OPND_CONST_INT)
                             ? (cond.data.intVal   == 0)
                             : (cond.data.floatVal == 0.0f);
                int taken = isZero; // IF_FALSE jumps when condition is 0 (false)
                int s0 = f->blocks[b].bb.succ[0]; // fall-through
                int s1 = f->blocks[b].bb.succ[1]; // taken (jump target)

                if (taken) {
                    // condition is always false → branch always taken
                    in->op   = IR_GOTO;
                    in->src1 = in->src2 = (Operand){ .kind = OPND_NONE };
                    f->blocks[b].bb.succ[0] = s1;
                    f->blocks[b].bb.succ[1] = -1;
                    if (s0 >= 0) f->blocks[s0].predCount--; // fall-through no longer reachable
                } else {
                    // condition is always true → branch never taken, delete IF_FALSE
                    eliminate[i] = 1;
                    f->blocks[b].bb.succ[1] = -1;
                    if (s1 >= 0) f->blocks[s1].predCount--; // target no longer reachable
                }
                modified = 1;
            } else if (!operand_equal(&cond, &in->src1)) {
                // condition not constant but simplified (e.g. propagated a copy)
                in->src1 = cond;
                modified = 1;
            }
            cp_transfer(in, &live, vm);
            continue;
        }

        /* ---- 3. Constant substitution + binary folding ---- */

        // try to replace src1/src2 with known constants
        Operand ns1 = const_map_try_fold(in->src1, &live, vm);
        Operand ns2 = const_map_try_fold(in->src2, &live, vm);
        if (!operand_equal(&ns1, &in->src1)) { in->src1 = ns1; modified = 1; }
        if (!operand_equal(&ns2, &in->src2)) { in->src2 = ns2; modified = 1; }

        // if both operands are now constants, try to fold the binary operation
        if (in->op != IR_IF_FALSE && in->op != IR_LABEL &&
            in->op != IR_GOTO     && in->op != IR_RETURN &&
            (in->op == IR_ADD || in->op == IR_SUB || in->op == IR_MUL ||
             in->op == IR_DIV || in->op == IR_MOD ||
             in->op == IR_LT  || in->op == IR_LE  || in->op == IR_GT  ||
             in->op == IR_GE  || in->op == IR_EQ  || in->op == IR_NE)) {

            if ((ns1.kind == OPND_CONST_INT || ns1.kind == OPND_CONST_FLOAT) &&
                (ns2.kind == OPND_CONST_INT || ns2.kind == OPND_CONST_FLOAT)) {
                modified |= cp_fold_binary(in, &ns1, &ns2);
            }
        }

        cp_transfer(in, &live, vm); // advance local map past this instruction
    }

    return modified;
}




int cp_optimize(IRFunction *f) {
    if (!f || f->blockCount == 0 || f->count == 0) return 0;

    int nBlocks = f->blockCount;
    Arena *arena = arena_create(0); // all dataflow storage lives here

    /* Pass 1: VarMap */
    VarMap vm;
    cp_build_varmap(f, &vm);
    int numVars = vm.nextId;

    /* Pass 2: allocate in[]/out[] — all initialised to LAT_UNKNOWN by const_map_init */
    ConstMap *in  = arena_alloc(arena, (size_t)nBlocks * sizeof(ConstMap));
    ConstMap *out = arena_alloc(arena, (size_t)nBlocks * sizeof(ConstMap));
    ConstMap  tmp;
    const_map_init(&tmp, numVars, arena);
    for (int b = 0; b < nBlocks; b++) {
        const_map_init(&in[b],  numVars, arena);
        const_map_init(&out[b], numVars, arena);
    }

    /* Pass 3: forward dataflow */
    cp_run_forward_dataflow(f, in, out, &tmp, numVars, &vm);

    /* Pass 4: rewrite + CFG pruning */
    int modified = 0;
    // eliminate[] is a boolean per-instruction: 1 = delete in sweep
    char *eliminate = arena_alloc(arena, (size_t)f->count * sizeof(char));
    memset(eliminate, 0, (size_t)f->count * sizeof(char));

    for (int b = 0; b < nBlocks; b++)
        modified |= cp_rewrite_block(f, b, in, eliminate, &vm, arena);

    // orphan labels left after CFG pruning can be removed safely
    modified |= (cp_mark_orphan_labels(f, eliminate) > 0);

    /* Pass 5: sweep — only if something was marked for elimination */
    if (modified)
        modified = ir_sweep(f, eliminate, nBlocks);

    arena_destroy(arena);
    varmap_destroy(&vm);
    return modified;
}
