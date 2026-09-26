#include <stdlib.h>
#include <string.h>
#include "svn.h"
#include "hash_table.h"

/**
 * @brief Compact key representing an IR operand.
 */
typedef struct {
    int kind;   /**< 0=VAR, 1=TEMP, 2=CONST_INT, 3=CONST_FLOAT, -1=other */
    union {
        struct { int varLevel; int varOffset; };
        int   tempId;
        int   intVal;
        float floatVal;
    } data;
} ValueKey;

/**
 * @brief Compact key representing an expression computation.
 */
typedef struct {
    int op;       /**< IROp cast to int */
    int vn1;      /**< Value number of first operand */
    int vn2;      /**< Value number of second operand */
} ExprKey;

/**
 * @brief List of operands holding a given value number.
 */
typedef struct {
    int     count;
    Operand names[SVN_MAX_NAMES];
} NameList;

/**
 * @brief Single scope in the sheaf-of-tables hierarchy.
 *
 * Three Hash_Table maps (operand->VN, expr->VN, VN->leaders), each with
 * its own arena-backed storage. Parent pointer forms the scope chain used
 * for upward lookups across an EBB.
 */
typedef struct SVNScope {
    Hash_Table       *operandToVn; /**< ValueKey  -> int (value number) */
    Hash_Table       *exprToVn;    /**< ExprKey   -> int (value number) */
    Hash_Table       *leaders;     /**< int (vn)  -> NameList */
    struct SVNScope  *parent;
} SVNScope;

/* ---- hash of raw bytes (FNV-1a 32-bit extended to unsigned long) -------- */

static unsigned long svn_hash_bytes(const void *key, size_t keySize) {
    const unsigned char *p = (const unsigned char *)key;
    unsigned long h = 2166136261ul;
    for (size_t i = 0; i < keySize; i++) {
        h ^= (unsigned long)p[i];
        h *= 16777619ul;
    }
    return h;
}

/* ---- key builders ------------------------------------------------------ */

static inline ValueKey svn_build_value_key(const Operand *op) {
    ValueKey key;
    memset(&key, 0, sizeof(key));
    switch (op->kind) {
    case OPND_VAR:
        key.kind           = 0;
        key.data.varLevel  = op->data.varLevel;
        key.data.varOffset = op->data.varOffset;
        break;
    case OPND_TEMP:
        key.kind        = 1;
        key.data.tempId = op->data.tempId;
        break;
    case OPND_CONST_INT:
        key.kind        = 2;
        key.data.intVal = op->data.intVal;
        break;
    case OPND_CONST_FLOAT:
        key.kind          = 3;
        key.data.floatVal = op->data.floatVal;
        break;
    default:
        key.kind = -1;
        break;
    }
    return key;
}

static inline ExprKey svn_build_expr_key(int operation, int valueNumber1, int valueNumber2) {
    ExprKey key;
    memset(&key, 0, sizeof(key));
    key.op  = operation;
    key.vn1 = valueNumber1;
    key.vn2 = valueNumber2;
    return key;
}

/* ---- table wrappers (ht_get/ht_set with typed keys) -------------------- */

static int operand_table_get(Hash_Table *t, const ValueKey *key, int *outVn) {
    return ht_get(t, (void *)key, sizeof(*key), outVn, sizeof(*outVn));
}

static void operand_table_set(Hash_Table *t, const ValueKey *key, int vn) {
    ht_set(t, (void *)key, sizeof(*key), &vn, sizeof(vn));
}

static int expr_table_get(Hash_Table *t, const ExprKey *key, int *outVn) {
    return ht_get(t, (void *)key, sizeof(*key), outVn, sizeof(*outVn));
}

static void expr_table_set(Hash_Table *t, const ExprKey *key, int vn) {
    ht_set(t, (void *)key, sizeof(*key), &vn, sizeof(vn));
}

static int leader_table_get(Hash_Table *t, int vn, NameList *outList) {
    return ht_get(t, &vn, sizeof(vn), outList, sizeof(*outList));
}

static void leader_table_set(Hash_Table *t, int vn, const NameList *list) {
    ht_set(t, &vn, sizeof(vn), (void *)list, sizeof(*list));
}

/* ---- scope lifecycle --------------------------------------------------- */

/** Small initial capacity: most EBB scopes hold few distinct values. */
#define SVN_HT_INIT_CAP 16

static void svn_scope_init(SVNScope *scope, SVNScope *parent) {
    scope->operandToVn = ht_create(SVN_HT_INIT_CAP, svn_hash_bytes);
    scope->exprToVn    = ht_create(SVN_HT_INIT_CAP, svn_hash_bytes);
    scope->leaders     = ht_create(SVN_HT_INIT_CAP, svn_hash_bytes);
    scope->parent      = parent;
}

static void svn_scope_destroy(SVNScope *scope) {
    if (scope->operandToVn) ht_destroy(scope->operandToVn, NULL);
    if (scope->exprToVn)    ht_destroy(scope->exprToVn, NULL);
    if (scope->leaders)     ht_destroy(scope->leaders, NULL);
    scope->operandToVn = scope->exprToVn = scope->leaders = NULL;
}

/* ---- core SVN helpers -------------------------------------------------- */

/**
 * @brief Adds a name as a leader for a value number.
 *
 * Copies the NameList from the nearest ancestor that already has one,
 * appends the new name, and stores the updated list in the current scope.
 */
static void svn_add_leader_for_value(int valueNumber, const Operand *name, SVNScope *scope) {
    NameList list;
    memset(&list, 0, sizeof(list));

    for (SVNScope *s = scope; s; s = s->parent)
        if (leader_table_get(s->leaders, valueNumber, &list)) break;

    if (list.count < SVN_MAX_NAMES)
        list.names[list.count++] = *name;
    leader_table_set(scope->leaders, valueNumber, &list);
}

/**
 * @brief Binds destination to a value number and records it as a leader.
 */
static void svn_define_value(const Operand *destination, int valueNumber, SVNScope *scope) {
    if (destination->kind != OPND_VAR && destination->kind != OPND_TEMP) return;
    ValueKey key = svn_build_value_key(destination);
    operand_table_set(scope->operandToVn, &key, valueNumber);
    svn_add_leader_for_value(valueNumber, destination, scope);
}

/**
 * @brief True if @p name still holds @p valueNumber (temps always valid).
 */
static int svn_is_leader_still_valid(const Operand *name, int valueNumber, SVNScope *scope) {
    if (name->kind != OPND_VAR) return 1;
    ValueKey key = svn_build_value_key(name);
    for (SVNScope *s = scope; s; s = s->parent) {
        int activeVn;
        if (operand_table_get(s->operandToVn, &key, &activeVn))
            return activeVn == valueNumber;
    }
    return 0;
}

/**
 * @brief Finds a still-valid leader operand for a value number.
 */
static int svn_find_valid_leader(int valueNumber, SVNScope *scope, Operand *outLeader) {
    NameList list;
    int found = 0;
    for (SVNScope *s = scope; s; s = s->parent)
        if (leader_table_get(s->leaders, valueNumber, &list)) { found = 1; break; }
    if (!found) return 0;

    for (int i = 0; i < list.count; i++) {
        if (svn_is_leader_still_valid(&list.names[i], valueNumber, scope)) {
            *outLeader = list.names[i];
            return 1;
        }
    }
    return 0;
}

/**
 * @brief Value number of an operand; assigns a fresh one if unseen in the chain.
 */
static int svn_value_number_of(Operand op, SVNScope *scope, int *vnCounter) {
    ValueKey key = svn_build_value_key(&op);
    if (key.kind < 0) return -1;

    int vn;
    for (SVNScope *s = scope; s; s = s->parent)
        if (operand_table_get(s->operandToVn, &key, &vn)) return vn;

    vn = (*vnCounter)++;
    operand_table_set(scope->operandToVn, &key, vn);
    svn_add_leader_for_value(vn, &op, scope);
    return vn;
}

/**
 * @brief Lookup expression; rewrite instruction to IR_ASSIGN from leader if hit.
 */
static void svn_lookup_or_insert_expr(IRInstr *instruction, const ExprKey *exprKey,
                                       SVNScope *scope, int *vnCounter) {
    int expressionVN;
    int isFound = 0;
    for (SVNScope *s = scope; s; s = s->parent)
        if (expr_table_get(s->exprToVn, exprKey, &expressionVN)) { isFound = 1; break; }

    Operand leaderOperand;
    if (isFound && svn_find_valid_leader(expressionVN, scope, &leaderOperand)) {
        instruction->op   = IR_ASSIGN;
        instruction->src1 = leaderOperand;
        instruction->src2 = (Operand){ .kind = OPND_NONE };
        svn_define_value(&instruction->dst, expressionVN, scope);
    } else {
        expressionVN = (*vnCounter)++;
        expr_table_set(scope->exprToVn, exprKey, expressionVN);
        svn_define_value(&instruction->dst, expressionVN, scope);
    }
}

static void svn_process_binary(IRInstr *instruction, SVNScope *scope, int *vnCounter) {
    int vn1 = svn_value_number_of(instruction->src1, scope, vnCounter);
    int vn2 = svn_value_number_of(instruction->src2, scope, vnCounter);

    if (ir_is_commutative(instruction->op) && vn1 > vn2) {
        int swapTemp = vn1;
        vn1 = vn2;
        vn2 = swapTemp;
    }

    ExprKey exprKey = svn_build_expr_key((int)instruction->op, vn1, vn2);
    svn_lookup_or_insert_expr(instruction, &exprKey, scope, vnCounter);
}

static void svn_process_unary(IRInstr *instruction, SVNScope *scope, int *vnCounter) {
    int vn1 = svn_value_number_of(instruction->src1, scope, vnCounter);
    ExprKey exprKey = svn_build_expr_key((int)instruction->op, vn1, -1);
    svn_lookup_or_insert_expr(instruction, &exprKey, scope, vnCounter);
}

static void svn_process_instr(IRInstr *instruction, SVNScope *scope, int *vnCounter) {
    switch (instruction->op) {
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_LT:  case IR_LE:  case IR_GT:  case IR_GE:  case IR_EQ: case IR_NE:
        svn_process_binary(instruction, scope, vnCounter);
        break;

    case IR_NEG: case IR_NOT: case IR_ITOF:
        svn_process_unary(instruction, scope, vnCounter);
        break;

    case IR_ASSIGN: {
        int sourceVN = svn_value_number_of(instruction->src1, scope, vnCounter);
        svn_define_value(&instruction->dst, sourceVN, scope);
        break;
    }

    case IR_LOAD_ARR:
    case IR_CALL: {
        int freshVN = (*vnCounter)++;
        svn_define_value(&instruction->dst, freshVN, scope);
        break;
    }

    case IR_GLOBAL_ADDR: {
        ExprKey exprKey = svn_build_expr_key((int)instruction->op,
                                             instruction->src1.data.globalOffset, -1);
        svn_lookup_or_insert_expr(instruction, &exprKey, scope, vnCounter);
        break;
    }

    case IR_STORE_ARR:
    case IR_PARAM:
    case IR_RETURN:
    case IR_GOTO:
    case IR_IF_FALSE:
    case IR_LABEL:
        break;
    }
}

/**
 * @brief Recursively processes an Extended Basic Block (EBB).
 */
static void svn_process_ebb(IRFunction *irFunction, int blockIndex, SVNScope *parentScope,
                            int *vnCounter, int *visitedBlocks) {
    visitedBlocks[blockIndex] = 1;

    SVNScope currentScope;
    svn_scope_init(&currentScope, parentScope);

    for (int instructionIndex = irFunction->blocks[blockIndex].bb.range.start;
         instructionIndex < irFunction->blocks[blockIndex].bb.range.end;
         instructionIndex++) {
        svn_process_instr(&irFunction->instrs[instructionIndex], &currentScope, vnCounter);
    }

    for (int successorIndex = 0; successorIndex < 2; successorIndex++) {
        int targetBlock = irFunction->blocks[blockIndex].bb.succ[successorIndex];
        if (targetBlock >= 0 && !visitedBlocks[targetBlock]
            && irFunction->blocks[targetBlock].predCount == 1) {
            svn_process_ebb(irFunction, targetBlock, &currentScope, vnCounter, visitedBlocks);
        }
    }

    svn_scope_destroy(&currentScope);
}

void svn_optimize(IRFunction *irFunction) {
    if (!irFunction || irFunction->blockCount == 0) return;

    int vnCounter = 0;
    int *visitedBlocks = calloc((size_t)irFunction->blockCount, sizeof(int));

    for (int blockIndex = 0; blockIndex < irFunction->blockCount; blockIndex++) {
        if (!visitedBlocks[blockIndex]
            && (blockIndex == 0 || irFunction->blocks[blockIndex].predCount != 1)) {
            svn_process_ebb(irFunction, blockIndex, NULL, &vnCounter, visitedBlocks);
        }
    }

    free(visitedBlocks);
}
