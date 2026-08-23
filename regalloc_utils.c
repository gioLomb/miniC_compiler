#include "regalloc_utils.h"

/* =========================================================================
 * Spill cost weighting
 * =========================================================================
 * Weight is 10^loopDepth (capped at depth 5 = 100 000) so that variables
 * live inside hot loops are strongly preferred for register allocation over
 * variables that are rarely executed. The register allocator sums these
 * weights across all def/use sites to build a per-vreg spill cost.
 * ========================================================================= */

int regalloc_spill_weight(int loopDepth) {
    static const int weights[] = {1, 10, 100, 1000, 10000, 100000};
    // clamp to the precomputed table range to avoid out-of-bounds access
    // and overflow for pathologically deep nesting
    if (loopDepth < 0) loopDepth = 0;
    if (loopDepth > 5) loopDepth = 5;
    return weights[loopDepth];
}

/* =========================================================================
 * Operand -> register-id helpers (file-internal)
 * =========================================================================
 * These mirror the sched_utils.h helpers but operate in the register-
 * allocator's id space: physical registers are represented as raw
 * MachPhysReg values (not offset by nextVreg). PHYS_AL is normalised to
 * PHYS_RAX before graph construction so the allocator never sees the alias.
 * ========================================================================= */

/**
 * @brief Extract the primary register id from a MachOperand (regalloc view).
 *
 * MO_VREG -> vregId
 * MO_PHYS -> physReg  (PHYS_AL normalised to PHYS_RAX)
 * MO_MEM  -> baseVreg of the SIB addressing mode
 * other   -> -1
 */
static inline int regalloc_operand_reg(const MachOperand *o) {
    switch (o->kind) {
    case MO_VREG: return o->vregId;
    // %al and %rax are the same architectural register at different
    // widths: normalising here avoids treating them as distinct nodes
    // in the interference graph
    case MO_PHYS: return (o->physReg == PHYS_AL) ? PHYS_RAX : o->physReg;
    case MO_MEM:  return (o->mem.baseVreg >= 0) ? o->mem.baseVreg : -1;
    default:      return -1;
    }
}

/**
 * @brief Extract the *index* register id from an MO_MEM operand.
 *
 * Returns `indexVreg` when the operand uses scaled-index addressing;
 * returns -1 for all other kinds or when `indexVreg` is absent.
 */
static inline int regalloc_operand_reg2(const MachOperand *o) {
    if (o->kind == MO_MEM && o->mem.indexVreg >= 0)
        return o->mem.indexVreg;
    return -1;
}

/* =========================================================================
 * instr_def — single register defined by an instruction
 * ========================================================================= */

int instr_def(const MachInstr *in, int nextVreg) {
    (void)nextVreg;
    switch (in->op) {
    // these opcodes never write a result register: comparisons only set
    // flags, control-flow/CALL/RET/PUSH/STORE/CQO/structural markers write
    // to memory, the stack, or nowhere at all
    case MACH_CMP: case MACH_TEST:
    case MACH_JMP: case MACH_JE: case MACH_JNE:
    case MACH_JL:  case MACH_JLE: case MACH_JG: case MACH_JGE:
    case MACH_CALL: case MACH_RET:
    case MACH_PUSH: case MACH_STORE:
    case MACH_CQO:
    case MACH_LABEL: case MACH_FUNC_BEGIN: case MACH_FUNC_END:
        return -1;
    default:
        // every other opcode writes its result into dst
        return regalloc_operand_reg(&in->dst);
    }
}

/* =========================================================================
 * instr_uses — explicit register reads
 * ========================================================================= */

/**
 * @brief Collect all registers explicitly read by @p in into @p out[].
 *
 * Fills @p out (caller provides space for at least LIVENESS_MAX_IDS ints)
 * with the ids of every register the instruction reads. Includes:
 *   - Primary and index registers of src1, src2.
 *   - For STORE: primary and index registers of dst.mem — STORE reads both
 *     the base and the index to compute the memory address.
 *     **[BUG FIX]** Previously only the base register was extracted for
 *     STORE, causing the interference graph to miss edges between the index
 *     vreg and other live vregs. With a single physical register assigned
 *     to both the index and a live variable, the store would silently
 *     corrupt the variable's value.
 *   - For PUSH, IDIV, CQO: primary register of dst (read before use).
 */
void instr_uses(const MachInstr *in, int nextVreg, int out[], int *n) {
    (void)nextVreg;
    *n = 0;
    int r;
    r = regalloc_operand_reg(&in->src1);  if (r >= 0) out[(*n)++] = r;
    r = regalloc_operand_reg2(&in->src1); if (r >= 0) out[(*n)++] = r;
    r = regalloc_operand_reg(&in->src2);  if (r >= 0) out[(*n)++] = r;
    r = regalloc_operand_reg2(&in->src2); if (r >= 0) out[(*n)++] = r;
    switch (in->op) {
    case MACH_STORE:
        r = regalloc_operand_reg(&in->dst);  if (r >= 0) out[(*n)++] = r;
        r = regalloc_operand_reg2(&in->dst); if (r >= 0) out[(*n)++] = r;
        break;
    case MACH_PUSH:
    case MACH_IDIV:
    case MACH_CQO:
        r = regalloc_operand_reg(&in->dst); if (r >= 0) out[(*n)++] = r;
        break;
    default:
        /* FIX: opcode RMW (ADD/SUB/IMUL/SAL/NEG/NOT/XOR) legge dst PRIMA
         * di scriverlo (es. "addq %src, %dst" equivale a dst = dst + src).
         * Senza questo, dst risulta "morto" subito dopo l'istruzione RMW
         * nella liveness all'indietro, e ig_build() non crea l'arco di
         * interferenza tra dst e un'altra variabile viva nella stessa
         * finestra — l'allocatore può sovrapporre lo stesso registro
         * fisico, corrompendo l'operando implicito in lettura a runtime. */
        if (regalloc_is_rmw(in->op)) {
            r = regalloc_operand_reg(&in->dst); if (r >= 0) out[(*n)++] = r;
        }
        break;
    }
#undef SCHED_TRY 
}
/* =========================================================================
 * instr_implicit_uses / instr_implicit_defs
 * =========================================================================
 * ABI-mandated implicit reads and writes not visible in the explicit
 * operands. These are added to the interference graph as if they were
 * explicit uses/defs so that the allocator reserves the correct physical
 * registers at call sites and around IDIV/CQO.
 * ========================================================================= */

void instr_implicit_uses(const MachInstr *in, int nextVreg, int out[], int *n) {
    *n = 0;
    switch (in->op) {
    case MACH_IDIV:
        /* IDIV reads RDX:RAX as the dividend */
        out[(*n)++] = nextVreg + PHYS_RAX;
        out[(*n)++] = nextVreg + PHYS_RDX;
        break;
    case MACH_CQO:
        /* CQO sign-extends RAX into RDX:RAX; reads RAX */
        out[(*n)++] = nextVreg + PHYS_RAX;
        break;
    case MACH_CALL:
        /* All caller-saved registers are potentially clobbered */
        // modelled conservatively as reads too: the callee may read them
        // as incoming arguments, so they must be excluded from reuse
        // across the call regardless of direction
        for (int p = 0; p < PHYS_CALLER_SAVED_COUNT; p++)
            out[(*n)++] = nextVreg + p;
        break;
    case MACH_RET:
        /* RET reads the return value from RAX */
        out[(*n)++] = nextVreg + PHYS_RAX;
        break;
    default: break;
    }
}

void instr_implicit_defs(const MachInstr *in, int nextVreg, int out[], int *n) {
    *n = 0;
    switch (in->op) {
    case MACH_IDIV:
        /* IDIV writes quotient -> RAX, remainder -> RDX */
        out[(*n)++] = nextVreg + PHYS_RAX;
        out[(*n)++] = nextVreg + PHYS_RDX;
        break;
    case MACH_CQO:
        /* CQO writes the sign-extension into RDX */
        out[(*n)++] = nextVreg + PHYS_RDX;
        break;
    case MACH_CALL:
        /* CALL clobbers all caller-saved registers */
        for (int p = 0; p < PHYS_CALLER_SAVED_COUNT; p++)
            out[(*n)++] = nextVreg + p;
        break;
    default: break;
    }
}

/* =========================================================================
 * instr_defs — explicit register writes (wrapper around instr_def)
 * ========================================================================= */

void instr_defs(const MachInstr *in, int nextVreg, int out[], int *n) {
    *n = 0;
    // thin wrapper: instr_def already returns -1 or exactly one id
    int id = instr_def(in, nextVreg);
    if (id >= 0) out[(*n)++] = id;
}

/* =========================================================================
 * Instruction predicates for the register allocator
 * ========================================================================= */

/**
 * @brief Return non-zero if @p op is a read-modify-write operation.
 *
 * RMW instructions read their `dst` operand before writing it (e.g.
 * `addq %rcx, %rax` reads and then writes RAX). The spill inserter uses
 * this to decide whether it must reload `dst` before emitting the instruction.
 */
int regalloc_is_rmw(MachOp op) {
    switch (op) {
    // two-operand ALU ops where dst is both an input and the output
    case MACH_ADD: case MACH_SUB: case MACH_IMUL:
    case MACH_SAL: case MACH_NEG: case MACH_NOT: case MACH_XOR:
        return 1;
    default: return 0;
    }
}

/**
 * @brief Return non-zero if @p op is a SETcc instruction.
 *
 * SETcc writes into %al (PHYS_AL), which is an alias of RAX. The
 * interference-graph builder adds a constraint excluding RAX from any
 * vreg live across a SETcc to avoid aliasing conflicts.
 */
int regalloc_is_setcc(MachOp op) {
    switch (op) {
    case MACH_SETE: case MACH_SETNE: case MACH_SETL:
    case MACH_SETLE: case MACH_SETG: case MACH_SETGE:
        return 1;
    default: return 0;
    }
}

/**
 * @brief Return non-zero if @p op is a control-transfer instruction.
 *
 * Used by the spill inserter to invalidate the reload cache at basic-block
 * boundaries: after a jump or call, execution may have come from a different
 * predecessor, so cached reload temporaries from the current block are stale.
 */
int regalloc_is_ctrl_transfer(MachOp op) {
    switch (op) {
    // any instruction that can transfer control away from the next
    // sequential instruction invalidates same-block assumptions
    case MACH_JMP: case MACH_JE: case MACH_JNE: case MACH_JL:
    case MACH_JLE: case MACH_JG: case MACH_JGE:
    case MACH_CALL: case MACH_RET:
        return 1;
    default: return 0;
    }
}