/**
 * @file ir_op_info.c
 * @brief Shared IR opcode / operand predicates used by optimizers and codegen.
 *
 * Split out of ir.c so classification lives in one place (class counterpart:
 * instr_query.c for MachInstr). Behavior is identical to the previous
 * implementations in ir.c / local helpers in cp.c.
 */

#include "ir.h"
#include <stdint.h>
#include <string.h>

int ir_is_terminator(IROp op) {
    /* Terminators always end a basic block. */
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
        (1U << IR_NE)   | (1U << IR_ASSIGN) | (1U << IR_GLOBAL_ADDR) |
        (1U << IR_ITOF);
    return (op < 32) && ((mask >> op) & 1U);
}

int ir_defines_dst(IROp op) {
    /* Opcodes that write a value into dst; used by liveness/DCE/CP. */
    static const uint32_t DEFINES_DST_MASK =
        (1u << IR_ADD)         | (1u << IR_SUB)  | (1u << IR_MUL)  |
        (1u << IR_DIV)         | (1u << IR_MOD)  | (1u << IR_NEG)  |
        (1u << IR_NOT)         | (1u << IR_LT)   | (1u << IR_LE)   |
        (1u << IR_GT)          | (1u << IR_GE)   | (1u << IR_EQ)   |
        (1u << IR_NE)          | (1u << IR_ASSIGN)                  |
        (1u << IR_GLOBAL_ADDR) | (1u << IR_LOAD_ARR) | (1u << IR_CALL) |
        (1u << IR_ITOF);

    return (op < 32) && ((DEFINES_DST_MASK >> op) & 1u);
}

int ir_is_commutative(IROp op) {
    static const unsigned int mask =
        (1U << IR_ADD) | (1U << IR_MUL) | (1U << IR_EQ) | (1U << IR_NE);
    return (mask >> op) & 1U;
}

int ir_is_comparison(IROp op) {
    static const unsigned int mask =
        (1U << IR_LT) | (1U << IR_LE) | (1U << IR_GT) |
        (1U << IR_GE) | (1U << IR_EQ) | (1U << IR_NE);
    return (op < 32) && ((mask >> op) & 1U);
}

int ir_is_binary_op(IROp op) {
    /* Arithmetic + relational binary ops (same set CP used for transfer). */
    static const unsigned int mask =
        (1U << IR_ADD) | (1U << IR_SUB) | (1U << IR_MUL) |
        (1U << IR_DIV) | (1U << IR_MOD) |
        (1U << IR_LT)  | (1U << IR_LE)  | (1U << IR_GT) |
        (1U << IR_GE)  | (1U << IR_EQ)  | (1U << IR_NE);
    return (op < 32) && ((mask >> op) & 1U);
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
