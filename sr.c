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
 * multiplication are nested inside conditionals within the loop body.
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
    int     incrementInstructionIndex; /**< Instruction index where the step is applied. */
} InductionBase;

/**
 * @brief Descriptor for a derived induction variable.
 *
 * Derived induction variables are defined as the multiplication of a basic
 * induction variable by a loop-invariant constant factor.
 */
typedef struct {
    int     multiplicationInstructionIndex; /**< Instruction index of the multiplication. */
    int     destinationVariableId;          /**< Unique variable ID for the target destination operand. */
    Operand destinationOperand;             /**< Target destination operand. */
    int     multiplierFactor;               /**< Constant multiplier scaling factor. */
    int     strideValue;                    /**< Calculated stride (step * multiplier). */
    int     strengthReductionTemporaryId;   /**< ID of the generated strength-reduction temporary. */
    int     baseInductionVariableIndex;     /**< Index into the parent basic induction variable array. */
} InductionDerived;

/* =========================================================================
 * Induction Variable Analysis Helpers
 * ========================================================================= */

static void count_variable_definitions(const IRFunction *irFunction, const Loop *targetLoop, 
                                       VarMap *variableMap, int *definitionCounts) {
    // walk all loop body blocks
    for (int bodyIndex = 0; bodyIndex < targetLoop->bodyCount; bodyIndex++) {
        int blockIndex = targetLoop->body[bodyIndex];
        for (int instructionIndex = irFunction->blocks[blockIndex].bb.range.start; 
             instructionIndex < irFunction->blocks[blockIndex].bb.range.end; 
             instructionIndex++) {
            const IRInstr *currentInstruction = &irFunction->instrs[instructionIndex];
            if (!ir_defines_dst(currentInstruction->op)) continue; // no dst, skip

            // bump write-count for tracked vars (untracked -> id -1, ignored)
            int variableId = varmap_operand_id(variableMap, currentInstruction->dst);
            if (variableId >= 0) definitionCounts[variableId]++;
        }
    }
}


static int collect_base_induction_vars(const IRFunction *irFunction, const Loop *targetLoop,
                                        VarMap *variableMap, const int *definitionCounts,
                                        InductionBase *baseVariables) {
    int collectedCount = 0;

    for (int bodyIndex = 0; bodyIndex < targetLoop->bodyCount && collectedCount < MAX_IVARS; bodyIndex++) {
        int blockIndex = targetLoop->body[bodyIndex];

        // cache block instruction range — avoids repeated struct dereference in inner loop
        int instrStart = irFunction->blocks[blockIndex].bb.range.start;
        int instrEnd   = irFunction->blocks[blockIndex].bb.range.end;

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

            baseVariables[collectedCount++] = (InductionBase){
                .variable                  = instr->dst,
                .variableId                = variableId,
                .stepValue                 = stepValue,
                .incrementInstructionIndex = iIdx,
            };
        }
    }

    return collectedCount;
}

/**
 * @brief Scans a loop body to locate basic induction variables.
 */
static int findInductionBase(IRFunction *irFunction, Loop *targetLoop, VarMap *variableMap,
                             InductionBase *baseVariables, Arena *arena) {
    int totalVariables = variableMap->nextId;
    // per-variable def count, arena-owned
    int *definitionCounts = arena_alloc(arena, (size_t)totalVariables * sizeof(int));
    memset(definitionCounts, 0, (size_t)totalVariables * sizeof(int));

    count_variable_definitions(irFunction, targetLoop, variableMap, definitionCounts);
    return collect_base_induction_vars(irFunction, targetLoop, variableMap, definitionCounts, baseVariables);
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

static int try_match_derived_iv(const IRInstr *currentInstruction, int instructionIndex, VarMap *variableMap,
                                const InductionBase *baseVariables, int baseVariableCount,
                                InductionDerived *outDerivedVariable, int *nextTemporaryId) {
    if (currentInstruction->op != IR_MUL) return 0;
    if (!ir_operand_is_storage(currentInstruction->dst.kind)) return 0;

    // try match against each known base IV
    for (int baseIndex = 0; baseIndex < baseVariableCount; baseIndex++) {
        const InductionBase *baseVar = &baseVariables[baseIndex];
        int constantFactor = 0;

        // i*const or const*i, commutative
        if (!match_iv_mul_const(currentInstruction, baseVar->variable, &constantFactor)) continue;

        int calculatedStride; // stride = step(i) * multiplier
        if (!foldInt(IR_MUL, baseVar->stepValue, constantFactor, &calculatedStride)) continue;

        int destinationId = varmap_operand_id(variableMap, currentInstruction->dst);
        if (destinationId < 0) continue;

        // valid match, alloc fresh temp id
        *outDerivedVariable = (InductionDerived){
            .multiplicationInstructionIndex = instructionIndex,
            .destinationVariableId          = destinationId,
            .destinationOperand             = currentInstruction->dst,
            .multiplierFactor               = constantFactor,
            .strideValue                    = calculatedStride,
            .strengthReductionTemporaryId   = (*nextTemporaryId)++,
            .baseInductionVariableIndex     = baseIndex,
        };
        return 1;
    }

    return 0;
}

/**
 * @brief Scans a loop body for derived induction variables based on basic IVs.
 */
static int findDerived(IRFunction *irFunction, Loop *targetLoop, VarMap *variableMap,
                       InductionBase *baseVariables, int baseVariableCount,
                       InductionDerived *derivedVariables, int *nextTemporaryId) {
    int derivedCount = 0;

    // same traversal as basic-IV scan
    for (int bodyIndex = 0; bodyIndex < targetLoop->bodyCount; bodyIndex++) {
        int blockIndex = targetLoop->body[bodyIndex];
        int start = irFunction->blocks[blockIndex].bb.range.start;
        int end   = irFunction->blocks[blockIndex].bb.range.end;

        for (int instrIdx = start; instrIdx < end && derivedCount < MAX_DERIVED; instrIdx++) {
            if (try_match_derived_iv(&irFunction->instrs[instrIdx], instrIdx, variableMap, 
                                     baseVariables, baseVariableCount, &derivedVariables[derivedCount], nextTemporaryId)) {
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
static inline IRInstr make_instr(IROp operation, Operand destination,
                                  Operand source1, Operand source2,
                                  int loopDepth) {
    return (IRInstr){ .op = operation, .dst = destination,
                      .src1 = source1, .src2 = source2,
                      .loopDepth = loopDepth };
}

/**
 * @brief Emit "t_sr = i * multiplier" for every derived induction variable.
 */
static int emit_preheader_inits(IRInstr *newInstructions, int currentInstructionCount,
                                 const InductionDerived *derivedVariables, int derivedVariableCount,
                                 const InductionBase *baseVariables, int outerLoopDepth) {
    for (int derivedIndex = 0; derivedIndex < derivedVariableCount; derivedIndex++) {
        const InductionDerived *derivedVar = &derivedVariables[derivedIndex];
        const InductionBase    *baseVar    = &baseVariables[derivedVar->baseInductionVariableIndex];
        
        // t_sr = i * multiplier, computed once in pre-header
        newInstructions[currentInstructionCount++] = make_instr(
            IR_MUL,
            (Operand){ .kind = OPND_TEMP, .data.tempId = derivedVar->strengthReductionTemporaryId },
            baseVar->variable,
            (Operand){ .kind = OPND_CONST_INT, .data.intVal = derivedVar->multiplierFactor },
            outerLoopDepth);
    }
    return currentInstructionCount;
}

static int patch_body_instruction(const IRInstr *currentInstruction, int originalInstructionIndex,
                                 const InductionDerived *derivedVariables, int derivedVariableCount,
                                 const int *multiplicationReplacementMap, const int *incrementIsBaseMap,
                                 int bodyLoopDepth, IRInstr *newInstructions, int currentInstructionCount) {
    int derivedIndex = multiplicationReplacementMap[originalInstructionIndex];

    if (derivedIndex >= 0) {
        // replace mul with copy from shadow temp
        newInstructions[currentInstructionCount++] = make_instr(
            IR_ASSIGN, derivedVariables[derivedIndex].destinationOperand,
            (Operand){ .kind = OPND_TEMP, .data.tempId = derivedVariables[derivedIndex].strengthReductionTemporaryId },
            (Operand){.kind = OPND_NONE}, currentInstruction->loopDepth);
    } else {
        newInstructions[currentInstructionCount++] = *currentInstruction; // keep as-is
    }

    int baseVariableIndex = incrementIsBaseMap[originalInstructionIndex];
    if (baseVariableIndex >= 0) {
        // base IV increment: splice "t_sr += stride" right after, per derived var
        for (int derivedSearchIndex = 0; derivedSearchIndex < derivedVariableCount; derivedSearchIndex++) {
            if (derivedVariables[derivedSearchIndex].baseInductionVariableIndex != baseVariableIndex) continue;
            
            Operand shadowTemporaryOperand = (Operand){ .kind = OPND_TEMP, .data.tempId = derivedVariables[derivedSearchIndex].strengthReductionTemporaryId };
            newInstructions[currentInstructionCount++] = make_instr(
                IR_ADD, shadowTemporaryOperand, shadowTemporaryOperand,
                (Operand){ .kind = OPND_CONST_INT, .data.intVal = derivedVariables[derivedSearchIndex].strideValue },
                bodyLoopDepth);
        }
    }

    return currentInstructionCount;
}

/**
 * @brief Rewrite loop-body instructions in place (into a new buffer).
 */
static int rewrite_loop_body(const IRFunction *irFunction, int totalOriginalInstructions, int insertionIndex,
                             const InductionDerived *derivedVariables, int derivedVariableCount,
                             const InductionBase *baseVariables,
                             const int *multiplicationReplacementMap, const int *incrementIsBaseMap,
                             int bodyLoopDepth, int outerLoopDepth,
                             IRInstr *newInstructions, int *oldToNewIndexMap,
                             int *preheaderInitStart, int *preheaderInitEnd) {
    int currentInstructionCount = 0;

    for (int instructionIndex = 0; instructionIndex < totalOriginalInstructions; instructionIndex++) {
        // splice pre-header inits right before loop header
        if (instructionIndex == insertionIndex) {
            *preheaderInitStart = currentInstructionCount;
            currentInstructionCount = emit_preheader_inits(newInstructions, currentInstructionCount,
                                                           derivedVariables, derivedVariableCount, baseVariables, outerLoopDepth);
            *preheaderInitEnd = currentInstructionCount;
        }

        // record old->new position, needed to remap block ranges later
        oldToNewIndexMap[instructionIndex] = currentInstructionCount;
        currentInstructionCount = patch_body_instruction(&irFunction->instrs[instructionIndex], instructionIndex,
                                                         derivedVariables, derivedVariableCount,
                                                         multiplicationReplacementMap, incrementIsBaseMap,
                                                         bodyLoopDepth, newInstructions, currentInstructionCount);
    }

    // edge case: insertion point at end of buffer, loop above never hit it
    if (insertionIndex >= totalOriginalInstructions) {
        *preheaderInitStart = currentInstructionCount;
        currentInstructionCount = emit_preheader_inits(newInstructions, currentInstructionCount,
                                                       derivedVariables, derivedVariableCount, baseVariables, outerLoopDepth);
        *preheaderInitEnd = currentInstructionCount;
    }

    return currentInstructionCount;
}

/**
 * @brief Recompute every block's [start,end) range after instruction count changes.
 */
static void remap_block_ranges(IRFunction *irFunction, int preheaderBlockIndex, int preheaderInitStart, int preheaderInitEnd,
                                const int *oldToNewIndexMap, int totalOriginalInstructions, int newTotalInstructions) {
    for (int blockIndex = 0; blockIndex < irFunction->blockCount; blockIndex++) {
        if (blockIndex == preheaderBlockIndex) {
            // pre-header range = newly emitted inits (didn't exist before)
            irFunction->blocks[blockIndex].bb.range.start = preheaderInitStart;
            irFunction->blocks[blockIndex].bb.range.end   = preheaderInitEnd;
            continue;
        }

        int oldStart = irFunction->blocks[blockIndex].bb.range.start;
        int oldEnd   = irFunction->blocks[blockIndex].bb.range.end;

        if (oldStart == oldEnd) {
            // empty block: anchor to next instr's new pos, or buffer end if last
            int anchorIndex = (oldStart < totalOriginalInstructions) ? oldToNewIndexMap[oldStart] : newTotalInstructions;
            irFunction->blocks[blockIndex].bb.range.start = irFunction->blocks[blockIndex].bb.range.end = anchorIndex;
            continue;
        }

        // normal block: remap start/end (end exclusive -> last valid idx + 1)
        irFunction->blocks[blockIndex].bb.range.start = oldToNewIndexMap[oldStart];
        irFunction->blocks[blockIndex].bb.range.end   = oldToNewIndexMap[oldEnd - 1] + 1;
    }
}

/**
 * @brief Performs the strength reduction rewrite on loop instructions.
 */
static int applyStrengthReduction(IRFunction *irFunction, Loop *targetLoop,
                                   InductionBase *baseVariables, int baseVariableCount,
                                   InductionDerived *derivedVariables, int derivedVariableCount,
                                   Arena *arena) {
    if (derivedVariableCount == 0) return 0;

    int preheaderBlockIndex = targetLoop->preHeader;
    int headerBlockIndex    = targetLoop->header;
    int totalOriginalInstructions = irFunction->count;
    int insertionIndex      = irFunction->blocks[headerBlockIndex].bb.range.start; // insert point

    // loopDepth for new instrs: body vs one level up (pre-header)
    int bodyLoopDepth  = (insertionIndex < totalOriginalInstructions) ? irFunction->instrs[insertionIndex].loopDepth : 0;
    int outerLoopDepth = bodyLoopDepth > 0 ? bodyLoopDepth - 1 : 0;

    // Worst case: every derived IV adds one init instr (pre-header) plus one
    // stride-update instr (body), on top of all original instructions.
    int maxNewInstructionsSize = totalOriginalInstructions + derivedVariableCount * 2;
    IRInstr *newInstructions   = malloc((size_t)maxNewInstructionsSize * sizeof(IRInstr));

    int *oldToNewIndexMap             = arena_alloc(arena, (size_t)totalOriginalInstructions * sizeof(int));
    int *multiplicationReplacementMap = arena_alloc(arena, (size_t)totalOriginalInstructions * sizeof(int));
    int *incrementIsBaseMap           = arena_alloc(arena, (size_t)totalOriginalInstructions * sizeof(int));
    
    // -1 sentinel means "this instruction index is not a replacement/increment site".
    memset(multiplicationReplacementMap, -1, (size_t)totalOriginalInstructions * sizeof(int));
    memset(incrementIsBaseMap,           -1, (size_t)totalOriginalInstructions * sizeof(int));
    
    // Reverse-index derived vars by the instruction they replace, and base
    // IVs by the instruction where their increment lives, for O(1) lookup
    // during the single linear pass in rewrite_loop_body/patch_body_instruction.
    for (int derivedIndex = 0; derivedIndex < derivedVariableCount; derivedIndex++) 
        multiplicationReplacementMap[derivedVariables[derivedIndex].multiplicationInstructionIndex] = derivedIndex;
        
    for (int baseIndex = 0; baseIndex < baseVariableCount; baseIndex++) 
        incrementIsBaseMap[baseVariables[baseIndex].incrementInstructionIndex] = baseIndex;

    // Initialize to avoid compiler warning (they will be set by rewrite_loop_body)
    int preheaderInitStart = 0, preheaderInitEnd = 0;
    int newInstructionCount = rewrite_loop_body(irFunction, totalOriginalInstructions, insertionIndex,
                                                derivedVariables, derivedVariableCount, baseVariables,
                                                multiplicationReplacementMap, incrementIsBaseMap,
                                                bodyLoopDepth, outerLoopDepth,
                                                newInstructions, oldToNewIndexMap,
                                                &preheaderInitStart, &preheaderInitEnd);

    // Swap in the rewritten instruction buffer, discarding the old one.
    free(irFunction->instrs);
    irFunction->instrs   = newInstructions;
    irFunction->count    = newInstructionCount;
    irFunction->capacity = newInstructionCount;

    remap_block_ranges(irFunction, preheaderBlockIndex, preheaderInitStart, preheaderInitEnd, 
                       oldToNewIndexMap, totalOriginalInstructions, newInstructionCount);

    // Instruction buffer changed underneath any "current block" cursor state.
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
    LivenessResult livenessResult = liveness_computeIr(irFunction, NULL, livenessArena);
    VarMap        *variableMap    = &livenessResult.varMap;

    // Scan the whole function once to find the highest temp id already in
    // use, so newly minted strength-reduction temps never collide with
    // existing ones.
    int nextTemporaryId = 0;
    for (int instructionIndex = 0; instructionIndex < irFunction->count; instructionIndex++) {
        const IRInstr *currentInstruction = &irFunction->instrs[instructionIndex];
        const Operand *operands[3] = { &currentInstruction->dst, &currentInstruction->src1, &currentInstruction->src2 };
        
        for (int operandIndex = 0; operandIndex < 3; operandIndex++)
            if (operands[operandIndex]->kind == OPND_TEMP && operands[operandIndex]->data.tempId >= nextTemporaryId)
                nextTemporaryId = operands[operandIndex]->data.tempId + 1;
    }

    int totalTransformationsApplied = 0;
    for (int loopIndex = 0; loopIndex < totalLoops; loopIndex++) {
        Loop *targetLoop = &detectedLoops[loopIndex];
        // Pre-header is required as the landing spot for "t_sr = i * mult" inits.
        if (targetLoop->preHeader < 0) loop_build_pre_header(irFunction, targetLoop);

        InductionBase   baseVariables[MAX_IVARS];
        InductionDerived derivedVariables[MAX_DERIVED];

        // No basic IV -> nothing to derive strength reduction from, skip loop.
        int baseVariableCount = findInductionBase(irFunction, targetLoop, variableMap, baseVariables, arenaScratch);
        if (baseVariableCount == 0) continue;

        // No derived IV -> no multiplication to replace, skip loop.
        int derivedVariableCount = findDerived(irFunction, targetLoop, variableMap, baseVariables, 
                                               baseVariableCount, derivedVariables, &nextTemporaryId);
        if (derivedVariableCount == 0) continue;

        totalTransformationsApplied += applyStrengthReduction(irFunction, targetLoop, baseVariables, baseVariableCount,
                                                              derivedVariables, derivedVariableCount, arenaScratch);
    }

    varmap_destroy(&livenessResult.varMap);
    arena_destroy(livenessArena);
    return totalTransformationsApplied > 0;
}