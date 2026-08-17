/**
 * @file sr.c
 * @brief Strength reduction optimization pass implementation.
 *
 * See sr.h for the module overview and transformation details.
 *
 * Internal organization
 * ---------------------
 *  foldInt                 - Integer constant folding helper.
 *  sameOperand             - Operand equivalence comparison.
 *  findInductionBase       - Identifies basic induction variables (i = i +/- c).
 *  findDerived             - Identifies derived induction variables (t = i * d).
 *  make_instr              - IR instruction factory helper.
 *  applyStrengthReduction  - Rewrites loop body and inserts pre-header initializers.
 *  sr_optimize             - Driver function for the pass.
 */

#include <stdlib.h>
#include <string.h>
#include "sr.h"
#include "loop.h"
#include "liveness.h"
#include "arena.h"

/* =========================================================================
 * Helper Functions & Data Structures
 * ========================================================================= */

/**
 * @brief Attempts to fold a binary integer arithmetic operation at compile time.
 *
 * @param op  IR opcode (IR_MUL, IR_ADD, or IR_SUB).
 * @param a   First integer operand.
 * @param b   Second integer operand.
 * @param res Output pointer for the result.
 * @return 1 if folding succeeded, 0 otherwise.
 */
static inline int foldInt(IROp op, int a, int b, int *res) {
    switch (op) {
    case IR_MUL: *res = a * b; return 1;
    case IR_ADD: *res = a + b; return 1;
    case IR_SUB: *res = a - b; return 1;
    default:                   return 0;
    }
}

/**
 * @brief Tests two operands for structural identity.
 *
 * @param a First operand.
 * @param b Second operand.
 * @return 1 if both operands reference the same variable or temporary, 0 otherwise.
 */
static inline int sameOperand(Operand a, Operand b) {
    if (a.kind != b.kind) return 0;
    if (a.kind == OPND_VAR)
        return a.data.varLevel  == b.data.varLevel &&
               a.data.varOffset == b.data.varOffset;
    if (a.kind == OPND_TEMP)
        return a.data.tempId == b.data.tempId;
    return 0;
}

/**
 * @brief Descriptor for a basic induction variable in a loop.
 *
 * Basic induction variables are modified inside the loop exclusively via addition
 * or subtraction of a loop-invariant constant.
 */
typedef struct {
    Operand var;        /**< Operand representing the basic induction variable. */
    int     varId;      /**< Unique variable ID from VarMap. */
    int     step;       /**< Constant step value added/subtracted per iteration. */
    int     incrInstr;  /**< Instruction index where the step is applied. */
} InductionBase;

/**
 * @brief Descriptor for a derived induction variable.
 *
 * Derived induction variables are defined as the multiplication of a basic
 * induction variable by a loop-invariant constant factor.
 */
typedef struct {
    int     mulInstr;   /**< Instruction index of the multiplication. */
    int     dstId;      /**< Unique variable ID for the target destination operand. */
    Operand dst;        /**< Target destination operand. */
    int     multiplier; /**< Constant multiplier scaling factor. */
    int     stride;     /**< Calculated stride (step * multiplier). */
    int     srTempId;   /**< ID of the generated strength-reduction temporary. */
    int     baseIdx;    /**< Index into the parent basic induction variable array. */
} InductionDerived;



/* =========================================================================
 * Induction Variable Analysis
 * ========================================================================= */

/**
 * @brief Scans a loop body to locate basic induction variables.
 *
 * Identifies variables defined exactly once within the loop matching `i = i +/- CONST`.
 *
 * @param f     IR function containing the loop.
 * @param L     Loop descriptor.
 * @param vm    Variable mapping table.
 * @param ivars Output array populated with discovered basic induction variables.
 * @param arena Scratch memory arena for intermediate allocations.
 * @return Number of basic induction variables found.
 */
static int findInductionBase(IRFunction *f, Loop *L, VarMap *vm,
                              InductionBase *ivars, Arena *arena) {
    int count   = 0;
    int numVars = vm->nextId;
    int *defCount = arena_alloc(arena, (size_t)numVars * sizeof(int));
    memset(defCount, 0, (size_t)numVars * sizeof(int));

    // Count definitions of each variable in the loop body
    for (int i = 0; i < L->bodyCount; i++) {
        int b = L->body[i];
        for (int j = f->blocks[b].bb.start; j < f->blocks[b].bb.end; j++) {
            IRInstr *in = &f->instrs[j];
            if (!ir_defines_dst(in->op)) continue;
            int id = varmap_operand_id(vm, in->dst);
            if (id >= 0) defCount[id]++;
        }
    }

    // Filter candidate basic induction variables (pattern: dst = dst +/- CONST)
    for (int i = 0; i < L->bodyCount && count < MAX_IVARS; i++) {
        int b = L->body[i];
        for (int j = f->blocks[b].bb.start;
             j < f->blocks[b].bb.end && count < MAX_IVARS;
             j++) {
            const IRInstr *in = &f->instrs[j];

            if (in->op != IR_ADD && in->op != IR_SUB)          continue;
            if (!ir_operand_is_storage(in->dst.kind))            continue;
            if (!sameOperand(in->dst, in->src1))               continue;
            if (in->src2.kind != OPND_CONST_INT)               continue;

            int id = varmap_operand_id(vm, in->dst);
            if (id < 0 || defCount[id] != 1)                   continue;

            int step = in->src2.data.intVal;
            if (in->op == IR_SUB) step = -step;

            ivars[count++] = (InductionBase){
                .var       = in->dst,
                .varId     = id,
                .step      = step,
                .incrInstr = j,
            };
        }
    }

    return count;
}

/**
 * @brief Scans a loop body for derived induction variables based on basic IVs.
 *
 * Looks for multiplications of the form `t = i * CONST` or `t = CONST * i`.
 *
 * @param f          IR function containing the loop.
 * @param L          Loop descriptor.
 * @param vm         Variable mapping table.
 * @param ivars      Array of basic induction variables in the loop.
 * @param ivarCount  Number of basic induction variables.
 * @param derived    Output array populated with derived induction variables.
 * @param nextTemp   Pointer to counter for generating new unique temporary IDs.
 * @return Number of derived induction variables found.
 */
static int findDerived(IRFunction *f, Loop *L, VarMap *vm,
                        InductionBase *ivars, int ivarCount,
                        InductionDerived *derived, int *nextTemp) {
    int count = 0;

    for (int i = 0; i < L->bodyCount; i++) {
        int b = L->body[i];
        for (int j = f->blocks[b].bb.start;
             j < f->blocks[b].bb.end && count < MAX_DERIVED;
             j++) {
            const IRInstr *in = &f->instrs[j];

            if (in->op != IR_MUL) continue;
            if (!ir_operand_is_storage(in->dst.kind)) continue;

            // Check whether either multiplication operand matches a known basic IV
            for (int v = 0; v < ivarCount; v++) {
                InductionBase *iv = &ivars[v];
                int d = 0;

                if (sameOperand(in->src1, iv->var) && in->src2.kind == OPND_CONST_INT)
                    d = in->src2.data.intVal;
                else if (sameOperand(in->src2, iv->var) && in->src1.kind == OPND_CONST_INT)
                    d = in->src1.data.intVal;
                else
                    continue;

                // Stride = basic_step * multiplier
                int stride;
                if (!foldInt(IR_MUL, iv->step, d, &stride)) continue;

                int dstId = varmap_operand_id(vm, in->dst);
                if (dstId < 0) continue;

                derived[count++] = (InductionDerived){
                    .mulInstr   = j,
                    .dstId      = dstId,
                    .dst        = in->dst,
                    .multiplier = d,
                    .stride     = stride,
                    .srTempId   = (*nextTemp)++,
                    .baseIdx    = v,
                };
                break;
            }
        }
    }

    return count;
}

/* =========================================================================
 * Code Transformation
 * ========================================================================= */

/**
 * @brief Constructs a zeroed IR instruction with essential fields initialized.
 *
 * @param op        IR opcode.
 * @param dst       Destination operand.
 * @param src1      First source operand.
 * @param src2      Second source operand.
 * @param loopDepth Loop nesting depth.
 * @return Fully initialized IRInstr structure.
 */
static inline IRInstr make_instr(IROp op, Operand dst,
                                  Operand src1, Operand src2,
                                  int loopDepth) {
    return (IRInstr){ .op = op, .dst = dst,
                      .src1 = src1, .src2 = src2,
                      .loopDepth = loopDepth };
}

/**
 * @brief Performs the strength reduction rewrite on loop instructions.
 *
 * 1. Emits pre-header initializers (`t_sr = i * multiplier`).
 * 2. Replaces loop body multiplications with copies (`t = t_sr`).
 * 3. Appends incremental updates (`t_sr = t_sr + stride`) following basic IV increments.
 *
 * @param f            IR function being transformed.
 * @param L            Target loop descriptor.
 * @param ivars        Array of basic induction variables.
 * @param ivarCount    Number of basic induction variables.
 * @param derived      Array of derived induction variables.
 * @param derivedCount Number of derived induction variables.
 * @return 1 if instructions were rewritten, 0 otherwise.
 */
static int applyStrengthReduction(IRFunction *f, Loop *L,
                                   InductionBase *ivars, int ivarCount,
                                   InductionDerived *derived, int derivedCount) {
    if (derivedCount == 0) return 0;

    int phIdx    = L->preHeader;
    int header   = L->header;
    int nInstrs  = f->count;
    int nBlocks  = f->blockCount;
    int insertAt = f->blocks[header].bb.start;

    // Retrieve loop body and outer nesting depths
    int bodyDepth  = f->instrs[insertAt].loopDepth;
    int outerDepth = bodyDepth > 0 ? bodyDepth - 1 : 0;

    int maxNew = nInstrs + derivedCount * (1 + ivarCount);
    IRInstr *newInstrs = malloc((size_t)maxNew * sizeof(IRInstr));
    int newCount = 0;

    int *oldToNew = malloc((size_t)nInstrs * sizeof(int));
    memset(oldToNew, -1, (size_t)nInstrs * sizeof(int));

    int phInitStart = -1, phInitEnd = -1;

    // Emit pre-header initialization instructions for derived temporaries
    #define EMIT_PH_INITS()                                                  \
        do {                                                                  \
            phInitStart = newCount;                                           \
            for (int d = 0; d < derivedCount; d++) {                         \
                InductionDerived *der = &derived[d];                         \
                InductionBase    *iv  = &ivars[der->baseIdx];                \
                newInstrs[newCount++] = make_instr(                          \
                    IR_MUL,                                                   \
                    (Operand){ .kind = OPND_TEMP,                            \
                               .data.tempId = der->srTempId },               \
                    iv->var,                                                  \
                    (Operand){ .kind = OPND_CONST_INT,                       \
                               .data.intVal = der->multiplier },             \
                    outerDepth);                                              \
            }                                                                 \
            phInitEnd = newCount;                                             \
        } while (0)

    for (int j = 0; j < nInstrs; j++) {

        // Emit pre-header initializers immediately before the loop header
        if (j == insertAt) EMIT_PH_INITS();

        IRInstr *in = &f->instrs[j];

        // Replace multiplication: t = i * d -> t = t_sr
        int isDerived = 0;
        for (int d = 0; d < derivedCount; d++) {
            if (j != derived[d].mulInstr) continue;
            oldToNew[j] = newCount;
            newInstrs[newCount++] = make_instr(
                IR_ASSIGN,
                derived[d].dst,
                (Operand){ .kind = OPND_TEMP,
                           .data.tempId = derived[d].srTempId },
                noOperand(),
                in->loopDepth);
            isDerived = 1;
            break;
        }
        if (!isDerived) {
            oldToNew[j] = newCount;
            newInstrs[newCount++] = *in;
        }

        // Insert step update after basic IV increment: t_sr = t_sr + stride
        for (int v = 0; v < ivarCount; v++) {
            if (j != ivars[v].incrInstr) continue;
            for (int d = 0; d < derivedCount; d++) {
                if (derived[d].baseIdx != v) continue;
                Operand srOp = (Operand){ .kind = OPND_TEMP,
                                          .data.tempId = derived[d].srTempId };
                newInstrs[newCount++] = make_instr(
                    IR_ADD, srOp, srOp,
                    (Operand){ .kind = OPND_CONST_INT,
                               .data.intVal = derived[d].stride },
                    bodyDepth);
            }
        }
    }

    // Handle empty loop edge case (insertAt == nInstrs)
    if (phInitStart == -1) EMIT_PH_INITS();

    #undef EMIT_PH_INITS

    free(f->instrs);
    f->instrs   = newInstrs;
    f->count    = newCount;
    f->capacity = newCount;

    // Update instruction bounds [start, end) for basic blocks
    for (int b = 0; b < nBlocks; b++) {
        if (b == phIdx) {
            f->blocks[b].bb.start = phInitStart;
            f->blocks[b].bb.end   = phInitEnd;
            continue;
        }
        int oldS = f->blocks[b].bb.start, oldE = f->blocks[b].bb.end;
        int newS = -1, newE = -1;
        for (int j = oldS; j < oldE; j++) {
            if (oldToNew[j] == -1) continue;
            if (newS == -1) newS = oldToNew[j];
            newE = oldToNew[j] + 1;
        }
        // Fallback for body blocks starting after insertion point without direct mapping
        if (newS == -1 && oldS >= insertAt) {
            newS = oldS + derivedCount;
            newE = oldE + derivedCount;
        }
        f->blocks[b].bb.start = (newS == -1) ? 0 : newS;
        f->blocks[b].bb.end   = (newE == -1) ? 0 : newE;
    }
    f->curBlockStart = 0;
    free(oldToNew);
    return 1;
}

/* =========================================================================
 * Public Interface
 * ========================================================================= */

int sr_optimize(IRFunction *f) {
    if (!f || f->blockCount == 0 || f->count == 0) return 0;

    int nBlocks = f->blockCount;
    int words   = (nBlocks + BITS_PER_WORD - 1) / BITS_PER_WORD;
    Arena *arena = arena_create(0);

    BitSet  *Dom  = loop_compute_dominators(f, words, arena);
    Loop    *loops  = arena_alloc(arena, MAX_LOOPS * sizeof(Loop));
    int      nLoops = loop_find(f, Dom, loops, arena);
    if (nLoops == 0) { arena_destroy(arena); return 0; }

    Arena         *livArena = arena_create(0);
    LivenessResult  liv     = liveness_computeIr(f, NULL, livArena);
    VarMap         *vm      = &liv.varMap;

    // Compute highest temporary ID in use across all function instructions
    int nextTemp = 0;
    for (int i = 0; i < f->count; i++) {
        const IRInstr *in = &f->instrs[i];
        const Operand *ops[3] = { &in->dst, &in->src1, &in->src2 };
        for (int k = 0; k < 3; k++) {
            if (ops[k]->kind == OPND_TEMP && ops[k]->data.tempId >= nextTemp)
                nextTemp = ops[k]->data.tempId + 1;
        }
    }

    int totalChanged = 0;
    for (int l = 0; l < nLoops; l++) {
        Loop *L = &loops[l];
        if (L->preHeader < 0) loop_build_pre_header(f, L);

        InductionBase    ivars[MAX_IVARS];
        InductionDerived derived[MAX_DERIVED];

        int ivarCount    = findInductionBase(f, L, vm, ivars, arena);
        if (ivarCount == 0) continue;

        int derivedCount = findDerived(f, L, vm, ivars, ivarCount, derived, &nextTemp);
        if (derivedCount == 0) continue;

        totalChanged += applyStrengthReduction(f, L, ivars, ivarCount,
                                                derived, derivedCount);
    }

    varmap_destroy(&liv.varMap);
    arena_destroy(livArena);
    arena_destroy(arena);
    return totalChanged > 0;
}