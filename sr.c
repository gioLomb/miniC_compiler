/**
 * @file sr.c
 * @brief Strength reduction optimization pass implementation.
 *
 * See sr.h for the module overview and transformation details.
 *
 * Internal organization
 * ---------------------
 *  foldInt                  - Integer constant folding helper.
 *  count_variable_definitions- Counts variable write frequencies within a loop.
 *  collect_base_induction_vars- Filters and extracts basic induction variables.
 *  findInductionBase        - Identifies basic induction variables (i = i +/- c).
 *  try_match_derived_iv     - Matches instructions against the derived IV pattern.
 *  findDerived              - Identifies derived induction variables (t = i * d).
 *  make_instr               - IR instruction factory helper.
 *  emit_preheader_inits     - Emits "t_sr = i * mult" for every derived IV.
 *  patch_body_instruction   - Replaces/appends instructions during body rewrite.
 *  rewrite_loop_body        - Rewrites the body, splicing stride updates in place.
 *  remap_block_ranges       - Recomputes block [start,end) after the rewrite.
 *  applyStrengthReduction   - Orchestrates the rewrite steps for one loop.
 *  sr_optimize              - Driver function for the pass.
 *
 * Why no dominance check is needed (unlike LICM)
 * -----------------------------------------------
 * LICM must verify a hoisted instruction's block dominates every loop exit,
 * because hoisting MOVES code to a different point in the CFG. SR never
 * moves anything out of its original block: the stride update
 * ("t_sr = t_sr + stride") is spliced immediately after the basic IV's own
 * increment instruction, in the very same basic block. Since a variable
 * with defCount == 1 has exactly one instruction that can change it, gluing
 * the shadow update right next to that instruction guarantees
 * "t_sr == i * multiplier" holds at every reachable program point after the
 * pre-header runs — regardless of how deeply the increment or the
 * mul are nested inside conditionals within the loop body.
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
static inline int foldInt(IROp operation, int operandA, int operandB, int *result) {
    switch (operation) {
    // only these 3 ops foldable
    case IR_MUL: *result = operandA * operandB; return 1;
    case IR_ADD: *result = operandA + operandB; return 1;
    case IR_SUB: *result = operandA - operandB; return 1;
    default:                                    return 0;
    }
}


/**
 * @brief Descriptor for a basic induction variable in a loop.
 *
 * Basic induction variables are modified inside the loop exclusively via addition
 * or subtraction of a loop-invariant constant.
 */
typedef struct {
    Operand variable;            /**< Operand representing the basic induction variable. */
    int     variableId;          /**< Unique variable ID from VarMap. */
    int     stepValue;           /**< Constant step value added/subtracted per iteration. */
    int     incrementInstrIdx; /**< Instr index where the step is applied. */
} InductionBase;

/**
 * @brief Descriptor for a derived induction variable.
 *
 * Derived induction variables are defined as the mul of a basic
 * induction variable by a loop-invariant constant factor.
 */
typedef struct {
    int     mulInstrIdx; /**< Instr index of the mul. */
    int     destVarId;          /**< Unique variable ID for the target dest operand. */
    Operand destOperand;             /**< Target dest operand. */
    int     multiplierFactor;               /**< Constant multiplier scaling factor. */
    int     strideValue;                    /**< Calculated stride (step * multiplier). */
    int     strengthReductionTempId;   /**< ID of the generated strength-reduction temporary. */
    int     baseInductionVarIdx;     /**< Idx into the parent basic induction variable array. */
} InductionDerived;

/* =========================================================================
 * Induction Var Analysis Helpers
 * ========================================================================= */

static void count_variable_definitions(const IRFunction *irFunction, const Loop *targetLoop, 
                                       VarMap *variableMap, int *definitionCounts) {
    // walk all loop body blocks
    for (int bodyIdx = 0; bodyIdx < targetLoop->bodyCount; bodyIdx++) {
        int blockIdx = targetLoop->body[bodyIdx];
        for (int instructionIdx = irFunction->blocks[blockIdx].bb.range.start; 
             instructionIdx < irFunction->blocks[blockIdx].bb.range.end; 
             instructionIdx++) {
            const IRInstr *currentInstr = &irFunction->instrs[instructionIdx];
            if (!ir_defines_dst(currentInstr->op)) continue; // no dst, skip

            // bump write-count for tracked vars (untracked -> id -1, ignored)
            int variableId = varmap_operand_id(variableMap, currentInstr->dst);
            if (variableId >= 0) definitionCounts[variableId]++;
        }
    }
}


static int collect_base_induction_vars(const IRFunction *irFunction, const Loop *targetLoop,
                                        VarMap *variableMap, const int *definitionCounts,
                                        InductionBase *baseVars) {
    int collectedCount = 0;

    for (int bodyIdx = 0; bodyIdx < targetLoop->bodyCount && collectedCount < MAX_IVARS; bodyIdx++) {
        int blockIdx = targetLoop->body[bodyIdx];

        // cache block instruction range — avoids repeated struct dereference in inner loop
        int instrStart = irFunction->blocks[blockIdx].bb.range.start;
        int instrEnd   = irFunction->blocks[blockIdx].bb.range.end;

        for (int iIdx = instrStart; iIdx < instrEnd && collectedCount < MAX_IVARS; iIdx++) {
            const IRInstr *instr = &irFunction->instrs[iIdx];

            // guard cascade: filter non-IV patterns (cold path)
            if (instr->op != IR_ADD && instr->op != IR_SUB)      continue;
            if (!ir_operand_is_storage(instr->dst.kind))          continue;
            if (!ir_is_same_operand(&(instr->dst), &(instr->src1)))            continue;
            if (instr->src2.kind != OPND_CONST_INT)               continue;

            // must be single-def (clean IV)
            int variableId = varmap_operand_id(variableMap, instr->dst);
            if (variableId < 0 || definitionCounts[variableId] != 1) continue;

            int stepValue = instr->src2.data.intVal;
            if (instr->op == IR_SUB) stepValue = -stepValue; // SUB → negative step

            baseVars[collectedCount++] = (InductionBase){
                .variable                  = instr->dst,
                .variableId                = variableId,
                .stepValue                 = stepValue,
                .incrementInstrIdx = iIdx,
            };
        }
    }

    return collectedCount;
}

/**
 * @brief Scans a loop body to locate basic induction variables.
 */
static int findInductionBase(IRFunction *irFunction, Loop *targetLoop, VarMap *variableMap,
                             InductionBase *baseVars, Arena *arena) {
    int totalVars = variableMap->nextId;
    // per-variable def count, arena-owned
    int *definitionCounts = arena_alloc(arena, (size_t)totalVars * sizeof(int));
    memset(definitionCounts, 0, (size_t)totalVars * sizeof(int));

    count_variable_definitions(irFunction, targetLoop, variableMap, definitionCounts);
    return collect_base_induction_vars(irFunction, targetLoop, variableMap, definitionCounts, baseVars);
}


// Returns 1 if instr matches (iv * const) or (const * iv), writing factor into *outFactor.
static int match_iv_mul_const(const IRInstr *in, Operand iv, int *outFactor) {
    if (ir_is_same_operand(&(in->src1), &iv) && in->src2.kind == OPND_CONST_INT) {
        *outFactor = in->src2.data.intVal; return 1;
    }
    if (ir_is_same_operand(&(in->src2), &iv) && in->src1.kind == OPND_CONST_INT) {
        *outFactor = in->src1.data.intVal; return 1;
    }
    return 0;
}

static int try_match_derived_iv(const IRInstr *currentInstr, int instructionIdx, VarMap *variableMap,
                                const InductionBase *baseVars, int baseVarCount,
                                InductionDerived *outDerivedVar, int *nextTempId) {
    if (currentInstr->op != IR_MUL) return 0;
    if (!ir_operand_is_storage(currentInstr->dst.kind)) return 0;

    // try match against each known base IV
    for (int baseIdx = 0; baseIdx < baseVarCount; baseIdx++) {
        const InductionBase *baseVar = &baseVars[baseIdx];
        int constantFactor = 0;

        // i*const or const*i, commutative
        if (!match_iv_mul_const(currentInstr, baseVar->variable, &constantFactor)) continue;

        int calculatedStride; // stride = step(i) * multiplier
        if (!foldInt(IR_MUL, baseVar->stepValue, constantFactor, &calculatedStride)) continue;

        int destId = varmap_operand_id(variableMap, currentInstr->dst);
        if (destId < 0) continue;

        // valid match, alloc fresh temp id
        *outDerivedVar = (InductionDerived){
            .mulInstrIdx = instructionIdx,
            .destVarId          = destId,
            .destOperand             = currentInstr->dst,
            .multiplierFactor               = constantFactor,
            .strideValue                    = calculatedStride,
            .strengthReductionTempId   = (*nextTempId)++,
            .baseInductionVarIdx     = baseIdx,
        };
        return 1;
    }

    return 0;
}

/**
 * @brief Scans a loop body for derived induction variables based on basic IVs.
 */
static int findDerived(IRFunction *irFunction, Loop *targetLoop, VarMap *variableMap,
                       InductionBase *baseVars, int baseVarCount,
                       InductionDerived *derivedVars, int *nextTempId) {
    int derivedCount = 0;

    // same traversal as basic-IV scan
    for (int bodyIdx = 0; bodyIdx < targetLoop->bodyCount; bodyIdx++) {
        int blockIdx = targetLoop->body[bodyIdx];
        int start = irFunction->blocks[blockIdx].bb.range.start;
        int end   = irFunction->blocks[blockIdx].bb.range.end;

        for (int instrIdx = start; instrIdx < end && derivedCount < MAX_DERIVED; instrIdx++) {
            if (try_match_derived_iv(&irFunction->instrs[instrIdx], instrIdx, variableMap, 
                                     baseVars, baseVarCount, &derivedVars[derivedCount], nextTempId)) {
                derivedCount++;
            }
        }
    }

    return derivedCount;
}

/* =========================================================================
 * Code Transformation Helpers
 * ========================================================================= */

/**
 * @brief Constructs a zeroed IR instruction with essential fields initialized.
 */
static inline IRInstr make_instr(IROp operation, Operand dest,
                                  Operand source1, Operand source2,
                                  int loopDepth) {
    return (IRInstr){ .op = operation, .dst = dest,
                      .src1 = source1, .src2 = source2,
                      .loopDepth = loopDepth };
}

/**
 * @brief Emit "t_sr = i * multiplier" for every derived induction variable.
 */
static int emit_preheader_inits(IRInstr *newInstrs, int currentInstrCount,
                                 const InductionDerived *derivedVars, int derivedVarCount,
                                 const InductionBase *baseVars, int outerLoopDepth) {
    for (int derivedIdx = 0; derivedIdx < derivedVarCount; derivedIdx++) {
        const InductionDerived *derivedVar = &derivedVars[derivedIdx];
        const InductionBase    *baseVar    = &baseVars[derivedVar->baseInductionVarIdx];
        
        // t_sr = i * multiplier, computed once in pre-header
        newInstrs[currentInstrCount++] = make_instr(
            IR_MUL,
            (Operand){ .kind = OPND_TEMP, .data.tempId = derivedVar->strengthReductionTempId },
            baseVar->variable,
            (Operand){ .kind = OPND_CONST_INT, .data.intVal = derivedVar->multiplierFactor },
            outerLoopDepth);
    }
    return currentInstrCount;
}

static int patch_body_instruction(const IRInstr *currentInstr, int originalInstrIdx,
                                 const InductionDerived *derivedVars, int derivedVarCount,
                                 const int *mulReplacementMap, const int *incrementIsBaseMap,
                                 int bodyLoopDepth, IRInstr *newInstrs, int currentInstrCount) {
    int derivedIdx = mulReplacementMap[originalInstrIdx];

    if (derivedIdx >= 0) {
        // replace mul with copy from shadow temp
        newInstrs[currentInstrCount++] = make_instr(
            IR_ASSIGN, derivedVars[derivedIdx].destOperand,
            (Operand){ .kind = OPND_TEMP, .data.tempId = derivedVars[derivedIdx].strengthReductionTempId },
            (Operand){.kind = OPND_NONE}, currentInstr->loopDepth);
    } else {
        newInstrs[currentInstrCount++] = *currentInstr; // keep as-is
    }

    int baseVarIdx = incrementIsBaseMap[originalInstrIdx];
    if (baseVarIdx >= 0) {
        // base IV increment: splice "t_sr += stride" right after, per derived var
        for (int derivedSearchIdx = 0; derivedSearchIdx < derivedVarCount; derivedSearchIdx++) {
            if (derivedVars[derivedSearchIdx].baseInductionVarIdx != baseVarIdx) continue;
            
            Operand shadowTempOperand = (Operand){ .kind = OPND_TEMP, .data.tempId = derivedVars[derivedSearchIdx].strengthReductionTempId };
            newInstrs[currentInstrCount++] = make_instr(
                IR_ADD, shadowTempOperand, shadowTempOperand,
                (Operand){ .kind = OPND_CONST_INT, .data.intVal = derivedVars[derivedSearchIdx].strideValue },
                bodyLoopDepth);
        }
    }

    return currentInstrCount;
}

/**
 * @brief Rewrite loop-body instructions in place (into a new buffer).
 */
static int rewrite_loop_body(const IRFunction *irFunction, int totalOriginalInstrs, int insertionIdx,
                             const InductionDerived *derivedVars, int derivedVarCount,
                             const InductionBase *baseVars,
                             const int *mulReplacementMap, const int *incrementIsBaseMap,
                             int bodyLoopDepth, int outerLoopDepth,
                             IRInstr *newInstrs, int *oldToNewIdxMap,
                             int *preheaderInitStart, int *preheaderInitEnd) {
    int currentInstrCount = 0;

    for (int instructionIdx = 0; instructionIdx < totalOriginalInstrs; instructionIdx++) {
        // splice pre-header inits right before loop header
        if (instructionIdx == insertionIdx) {
            *preheaderInitStart = currentInstrCount;
            currentInstrCount = emit_preheader_inits(newInstrs, currentInstrCount,
                                                           derivedVars, derivedVarCount, baseVars, outerLoopDepth);
            *preheaderInitEnd = currentInstrCount;
        }

        // record old->new position, needed to remap block ranges later
        oldToNewIdxMap[instructionIdx] = currentInstrCount;
        currentInstrCount = patch_body_instruction(&irFunction->instrs[instructionIdx], instructionIdx,
                                                         derivedVars, derivedVarCount,
                                                         mulReplacementMap, incrementIsBaseMap,
                                                         bodyLoopDepth, newInstrs, currentInstrCount);
    }

    // edge case: insertion point at end of buffer, loop above never hit it
    if (insertionIdx >= totalOriginalInstrs) {
        *preheaderInitStart = currentInstrCount;
        currentInstrCount = emit_preheader_inits(newInstrs, currentInstrCount,
                                                       derivedVars, derivedVarCount, baseVars, outerLoopDepth);
        *preheaderInitEnd = currentInstrCount;
    }

    return currentInstrCount;
}

/**
 * @brief Recompute every block's [start,end) range after instruction count changes.
 */
static void remap_block_ranges(IRFunction *irFunction, int preheaderblockIdx, int preheaderInitStart, int preheaderInitEnd,
                                const int *oldToNewIdxMap, int totalOriginalInstrs, int newTotalInstrs) {
    for (int blockIdx = 0; blockIdx < irFunction->blockCount; blockIdx++) {
        if (blockIdx == preheaderblockIdx) {
            // pre-header range = newly emitted inits (didn't exist before)
            irFunction->blocks[blockIdx].bb.range.start = preheaderInitStart;
            irFunction->blocks[blockIdx].bb.range.end   = preheaderInitEnd;
            continue;
        }

        int oldStart = irFunction->blocks[blockIdx].bb.range.start;
        int oldEnd   = irFunction->blocks[blockIdx].bb.range.end;

        if (oldStart == oldEnd) {
            // empty block: anchor to next instr's new pos, or buffer end if last
            int anchorIdx = (oldStart < totalOriginalInstrs) ? oldToNewIdxMap[oldStart] : newTotalInstrs;
            irFunction->blocks[blockIdx].bb.range.start = irFunction->blocks[blockIdx].bb.range.end = anchorIdx;
            continue;
        }

        // normal block: remap start/end (end exclusive -> last valid idx + 1)
        irFunction->blocks[blockIdx].bb.range.start = oldToNewIdxMap[oldStart];
        irFunction->blocks[blockIdx].bb.range.end   = oldToNewIdxMap[oldEnd - 1] + 1;
    }
}

/**
 * @brief Performs the strength reduction rewrite on loop instructions.
 */
static int applyStrengthReduction(IRFunction *irFunction, Loop *targetLoop,
                                   InductionBase *baseVars, int baseVarCount,
                                   InductionDerived *derivedVars, int derivedVarCount,
                                   Arena *arena) {
    if (derivedVarCount == 0) return 0;

    int preheaderblockIdx = targetLoop->preHeader;
    int headerblockIdx    = targetLoop->header;
    int totalOriginalInstrs = irFunction->count;
    int insertionIdx      = irFunction->blocks[headerblockIdx].bb.range.start; // insert point

    // loopDepth for new instrs: body vs one level up (pre-header)
    int bodyLoopDepth  = (insertionIdx < totalOriginalInstrs) ? irFunction->instrs[insertionIdx].loopDepth : 0;
    int outerLoopDepth = bodyLoopDepth > 0 ? bodyLoopDepth - 1 : 0;

    // Worst case: every derived IV adds one init instr (pre-header) plus one
    // stride-update instr (body), on top of all original instructions.
    int maxNewInstrsSize = totalOriginalInstrs + derivedVarCount * 2;
    IRInstr *newInstrs   = malloc((size_t)maxNewInstrsSize * sizeof(IRInstr));

    int *oldToNewIdxMap             = arena_alloc(arena, (size_t)totalOriginalInstrs * sizeof(int));
    int *mulReplacementMap = arena_alloc(arena, (size_t)totalOriginalInstrs * sizeof(int));
    int *incrementIsBaseMap           = arena_alloc(arena, (size_t)totalOriginalInstrs * sizeof(int));
    
    // -1 sentinel means "this instruction index is not a replacement/increment site".
    memset(mulReplacementMap, -1, (size_t)totalOriginalInstrs * sizeof(int));
    memset(incrementIsBaseMap,           -1, (size_t)totalOriginalInstrs * sizeof(int));
    
    // Reverse-index derived vars by the instruction they replace, and base
    // IVs by the instruction where their increment lives, for O(1) lookup
    // during the single linear pass in rewrite_loop_body/patch_body_instruction.
    for (int derivedIdx = 0; derivedIdx < derivedVarCount; derivedIdx++) 
        mulReplacementMap[derivedVars[derivedIdx].mulInstrIdx] = derivedIdx;
        
    for (int baseIdx = 0; baseIdx < baseVarCount; baseIdx++) 
        incrementIsBaseMap[baseVars[baseIdx].incrementInstrIdx] = baseIdx;

    // Initialize to avoid compiler warning (they will be set by rewrite_loop_body)
    int preheaderInitStart = 0, preheaderInitEnd = 0;
    int newInstrCount = rewrite_loop_body(irFunction, totalOriginalInstrs, insertionIdx,
                                                derivedVars, derivedVarCount, baseVars,
                                                mulReplacementMap, incrementIsBaseMap,
                                                bodyLoopDepth, outerLoopDepth,
                                                newInstrs, oldToNewIdxMap,
                                                &preheaderInitStart, &preheaderInitEnd);

    // Swap in the rewritten instruction buffer, discarding the old one.
    free(irFunction->instrs);
    irFunction->instrs   = newInstrs;
    irFunction->count    = newInstrCount;
    irFunction->capacity = newInstrCount;

    remap_block_ranges(irFunction, preheaderblockIdx, preheaderInitStart, preheaderInitEnd, 
                       oldToNewIdxMap, totalOriginalInstrs, newInstrCount);

    // Instr buffer changed underneath any "current block" cursor state.
    irFunction->curBlockStart = 0;
    return 1;
}

/* =========================================================================
 * Public Interface
 * ========================================================================= */

int sr_optimize(IRFunction *irFunction, Arena *arenaScratch) {
    if (!irFunction || irFunction->blockCount == 0 || irFunction->count == 0) return 0;

    int totalBlocks = irFunction->blockCount;
    int bitsetWords = (totalBlocks + BITS_PER_WORD - 1) / BITS_PER_WORD;
    arena_reset(arenaScratch);

    // Loop detection needs dominator info first (natural loops = back-edge + dominance).
    BitSet  *dominatorTree = loop_compute_dominators(irFunction, bitsetWords, arenaScratch);
    Loop    *detectedLoops = arena_alloc(arenaScratch, MAX_LOOPS * sizeof(Loop));
    int      totalLoops    = loop_find(irFunction, dominatorTree, detectedLoops, arenaScratch);
    if (totalLoops == 0) return 0;

    // VarMap (from liveness analysis) gives a dense variable-id space used
    // throughout to count/track definitions per variable.
    Arena         *livenessArena  = arena_create(0);
    LivenessResult livenessResult = liveness_computeIr(irFunction, NULL, NULL, livenessArena);
    VarMap        *variableMap    = &livenessResult.varMap;

    // Scan the whole function once to find the highest temp id already in
    // use, so newly minted strength-reduction temps never collide with
    // existing ones.
    int nextTempId = 0;
    for (int instructionIdx = 0; instructionIdx < irFunction->count; instructionIdx++) {
        const IRInstr *currentInstr = &irFunction->instrs[instructionIdx];
        const Operand *operands[3] = { &currentInstr->dst, &currentInstr->src1, &currentInstr->src2 };
        
        for (int operandIdx = 0; operandIdx < 3; operandIdx++)
            if (operands[operandIdx]->kind == OPND_TEMP && operands[operandIdx]->data.tempId >= nextTempId)
                nextTempId = operands[operandIdx]->data.tempId + 1;
    }

    int totalTransformationsApplied = 0;
    for (int loopIdx = 0; loopIdx < totalLoops; loopIdx++) {
        Loop *targetLoop = &detectedLoops[loopIdx];
        // Pre-header is required as the landing spot for "t_sr = i * mult" inits.
        if (targetLoop->preHeader < 0) loop_build_pre_header(irFunction, targetLoop);

        InductionBase   baseVars[MAX_IVARS];
        InductionDerived derivedVars[MAX_DERIVED];

        // No basic IV -> nothing to derive strength reduction from, skip loop.
        int baseVarCount = findInductionBase(irFunction, targetLoop, variableMap, baseVars, arenaScratch);
        if (baseVarCount == 0) continue;

        // No derived IV -> no mul to replace, skip loop.
        int derivedVarCount = findDerived(irFunction, targetLoop, variableMap, baseVars, 
                                               baseVarCount, derivedVars, &nextTempId);
        if (derivedVarCount == 0) continue;

        totalTransformationsApplied += applyStrengthReduction(irFunction, targetLoop, baseVars, baseVarCount,
                                                              derivedVars, derivedVarCount, arenaScratch);
    }

    varmap_destroy(&livenessResult.varMap);
    arena_destroy(livenessArena);
    return totalTransformationsApplied > 0;
}