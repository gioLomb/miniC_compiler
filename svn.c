#include <stdlib.h>
#include <string.h>
#include "svn.h"

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

/** Fixed inline capacity before an entry table spills to heap overflow. */
#define SVN_SCOPE_INLINE_CAP 8

/** operandToVn entry: raw-byte ValueKey -> value number. */
typedef struct { ValueKey key; int vn; } OperandVnEntry;
/** exprToVn entry: raw-byte ExprKey -> value number. */
typedef struct { ExprKey  key; int vn; } ExprVnEntry;
/** leaders entry: value number -> its NameList. */
typedef struct { int vn; NameList list; } LeaderEntry;

/**
 * @brief Small per-scope table: first SVN_SCOPE_INLINE_CAP entries live
 *        inline (no allocation); beyond that, spills into a heap-growable
 *        overflow array. Replaces a per-scope Hash_Table so no
 *        arena_create/ht_create churns per basic block (see svn_scope_init).
 */
typedef struct {
    int             count;         /**< Total entries (inline + overflow). */
    OperandVnEntry  inlineArr[SVN_SCOPE_INLINE_CAP];
    OperandVnEntry *overflow;      /**< NULL until count > SVN_SCOPE_INLINE_CAP. */
    int             overflowCap;
} OperandVnTable;

typedef struct {
    int          count;
    ExprVnEntry  inlineArr[SVN_SCOPE_INLINE_CAP];
    ExprVnEntry *overflow;
    int          overflowCap;
} ExprVnTable;

typedef struct {
    int          count;
    LeaderEntry  inlineArr[SVN_SCOPE_INLINE_CAP];
    LeaderEntry *overflow;
    int          overflowCap;
} LeaderTable;

/** @brief Entry at logical index i, whether it lives inline or in overflow. */
static inline OperandVnEntry *operand_table_at(OperandVnTable *t, int i) {
    return (i < SVN_SCOPE_INLINE_CAP) ? &t->inlineArr[i] : &t->overflow[i - SVN_SCOPE_INLINE_CAP];
}
static inline ExprVnEntry *expr_table_at(ExprVnTable *t, int i) {
    return (i < SVN_SCOPE_INLINE_CAP) ? &t->inlineArr[i] : &t->overflow[i - SVN_SCOPE_INLINE_CAP];
}
static inline LeaderEntry *leader_table_at(LeaderTable *t, int i) {
    return (i < SVN_SCOPE_INLINE_CAP) ? &t->inlineArr[i] : &t->overflow[i - SVN_SCOPE_INLINE_CAP];
}

static int operand_table_get(OperandVnTable *t, const ValueKey *key, int *outVn) {
    for (int i = 0; i < t->count; i++) {
        OperandVnEntry *e = operand_table_at(t, i);
        if (memcmp(&e->key, key, sizeof(*key)) == 0) { *outVn = e->vn; return 1; }
    }
    return 0;
}

/** @brief Update in place if key exists in THIS scope, else append (inline, or overflow on spill). */
static void operand_table_set(OperandVnTable *t, const ValueKey *key, int vn) {
    for (int i = 0; i < t->count; i++) {
        OperandVnEntry *e = operand_table_at(t, i);
        if (memcmp(&e->key, key, sizeof(*key)) == 0) { e->vn = vn; return; }
    }
    if (t->count >= SVN_SCOPE_INLINE_CAP) {
        int idx = t->count - SVN_SCOPE_INLINE_CAP;
        if (idx >= t->overflowCap) {
            t->overflowCap = t->overflowCap ? t->overflowCap * 2 : 8;
            t->overflow = realloc(t->overflow, (size_t)t->overflowCap * sizeof(OperandVnEntry));
            if (!t->overflow) abort();
        }
    }
    OperandVnEntry *slot = operand_table_at(t, t->count);
    slot->key = *key;
    slot->vn  = vn;
    t->count++;
}

static int expr_table_get(ExprVnTable *t, const ExprKey *key, int *outVn) {
    for (int i = 0; i < t->count; i++) {
        ExprVnEntry *e = expr_table_at(t, i);
        if (memcmp(&e->key, key, sizeof(*key)) == 0) { *outVn = e->vn; return 1; }
    }
    return 0;
}

static void expr_table_set(ExprVnTable *t, const ExprKey *key, int vn) {
    for (int i = 0; i < t->count; i++) {
        ExprVnEntry *e = expr_table_at(t, i);
        if (memcmp(&e->key, key, sizeof(*key)) == 0) { e->vn = vn; return; }
    }
    if (t->count >= SVN_SCOPE_INLINE_CAP) {
        int idx = t->count - SVN_SCOPE_INLINE_CAP;
        if (idx >= t->overflowCap) {
            t->overflowCap = t->overflowCap ? t->overflowCap * 2 : 8;
            t->overflow = realloc(t->overflow, (size_t)t->overflowCap * sizeof(ExprVnEntry));
            if (!t->overflow) abort();
        }
    }
    ExprVnEntry *slot = expr_table_at(t, t->count);
    slot->key = *key;
    slot->vn  = vn;
    t->count++;
}

static int leader_table_get(LeaderTable *t, int vn, NameList *outList) {
    for (int i = 0; i < t->count; i++) {
        LeaderEntry *e = leader_table_at(t, i);
        if (e->vn == vn) { *outList = e->list; return 1; }
    }
    return 0;
}

/** @brief Update in place if vn already has a list in THIS scope, else append. */
static void leader_table_set(LeaderTable *t, int vn, const NameList *list) {
    for (int i = 0; i < t->count; i++) {
        LeaderEntry *e = leader_table_at(t, i);
        if (e->vn == vn) { e->list = *list; return; }
    }
    if (t->count >= SVN_SCOPE_INLINE_CAP) {
        int idx = t->count - SVN_SCOPE_INLINE_CAP;
        if (idx >= t->overflowCap) {
            t->overflowCap = t->overflowCap ? t->overflowCap * 2 : 8;
            t->overflow = realloc(t->overflow, (size_t)t->overflowCap * sizeof(LeaderEntry));
            if (!t->overflow) abort();
        }
    }
    LeaderEntry *slot = leader_table_at(t, t->count);
    slot->vn   = vn;
    slot->list = *list;
    t->count++;
}

/**
 * @brief Represents a single scope in the sheaf-of-tables hierarchy.
 * Backed by fixed-inline-capacity tables (see SVN_SCOPE_INLINE_CAP): no
 * heap allocation for the common case (<=8 distinct entries per category
 * per scope), only rare overflow blocks spill to malloc/realloc.
 */
typedef struct SVNScope {
    OperandVnTable    operandToVn; /**< Operand identity -> Value Number */
    ExprVnTable       exprToVn;    /**< Expression shape  -> Value Number */
    LeaderTable       leaders;     /**< Value Number      -> NameList */
    struct SVNScope   *parent;     /**< Enclosing parent scope */
} SVNScope;

/**
 * @brief Constructs a normalized ValueKey for an IR operand.
 *
 * Normalizes VAR, TEMP, CONST_INT, and CONST_FLOAT operands into a
 * compact key suitable for table lookups. memset zeroes padding so
 * memcmp-based equality is well-defined.
 *
 * @param op Operand to normalize.
 * @return ValueKey structure (kind = -1 for unsupported operands).
 */
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
        key.kind           = 1;
        key.data.tempId    = op->data.tempId;
        break;
    case OPND_CONST_INT:
        key.kind           = 2;
        key.data.intVal    = op->data.intVal;
        break;
    case OPND_CONST_FLOAT:
        key.kind           = 3;
        key.data.floatVal  = op->data.floatVal;
        break;
    default:
        key.kind           = -1;  /* Not representable (e.g. NONE, global addr) */
        break;
    }
    return key;
}

/**
 * @brief Constructs an ExprKey for an expression.
 *
 * Keys an expression by its opcode and the value numbers of its operands.
 *
 * @param operation Opcode cast to int.
 * @param valueNumber1 Value number of first operand.
 * @param valueNumber2 Value number of second operand (-1 for unary).
 * @return ExprKey structure.
 */
static inline ExprKey svn_build_expr_key(int operation, int valueNumber1, int valueNumber2) {
    ExprKey key;
    memset(&key, 0, sizeof(key));
    key.op  = operation;
    key.vn1 = valueNumber1;
    key.vn2 = valueNumber2;
    return key;
}

/**
 * @brief Initialises a new SVN scope: zeroes all three tables (no
 *        allocation happens here — see SVNScope doc).
 */
static void svn_scope_init(SVNScope *scope, SVNScope *parent) {
    memset(&scope->operandToVn, 0, sizeof(scope->operandToVn));
    memset(&scope->exprToVn,    0, sizeof(scope->exprToVn));
    memset(&scope->leaders,     0, sizeof(scope->leaders));
    scope->parent = parent;
}

/** @brief Frees only the (rare) overflow blocks; inline storage is on-stack. */
static inline void svn_scope_destroy(SVNScope *scope) {
    free(scope->operandToVn.overflow);
    free(scope->exprToVn.overflow);
    free(scope->leaders.overflow);
}

/**
 * @brief Adds a name (operand) as a leader for a value number.
 *
 * Retrieves the existing NameList from the nearest ancestor scope that
 * contains it, appends the new name, and stores the updated list in the
 * current scope (shadowing ancestors).
 */
static void svn_add_leader_for_value(int valueNumber, const Operand *name, SVNScope *scope) {
    NameList list;
    memset(&list, 0, sizeof(list));

    for (SVNScope *s = scope; s; s = s->parent)
        if (leader_table_get(&s->leaders, valueNumber, &list)) break;

    if (list.count < SVN_MAX_NAMES) list.names[list.count++] = *name;
    leader_table_set(&scope->leaders, valueNumber, &list);
}

/**
 * @brief Associates a destination operand with a value number.
 *
 * Binds the operand to its VN in the operand->VN table and adds it as a
 * leader for that VN.
 */
static void svn_define_value(const Operand *destination, int valueNumber, SVNScope *scope) {
    if (destination->kind != OPND_VAR && destination->kind != OPND_TEMP) return;
    ValueKey key = svn_build_value_key(destination);
    operand_table_set(&scope->operandToVn, &key, valueNumber);
    svn_add_leader_for_value(valueNumber, destination, scope);
}

/**
 * @brief Checks if a name (variable) still holds a given value number.
 *
 * Temporaries are always valid (single definition per path).
 */
static int svn_is_leader_still_valid(const Operand *name, int valueNumber, SVNScope *scope) {
    if (name->kind != OPND_VAR) return 1;
    ValueKey key = svn_build_value_key(name);
    for (SVNScope *s = scope; s; s = s->parent) {
        int activeVn;
        if (operand_table_get(&s->operandToVn, &key, &activeVn))
            return activeVn == valueNumber;
    }
    return 0;
}

/**
 * @brief Finds a valid leader operand for a value number.
 */
static int svn_find_valid_leader(int valueNumber, SVNScope *scope, Operand *outLeader) {
    NameList list;
    int found = 0;
    for (SVNScope *s = scope; s; s = s->parent)
        if (leader_table_get(&s->leaders, valueNumber, &list)) { found = 1; break; }
    if (!found) return 0;

    for (int i = 0; i < list.count; i++)
        if (svn_is_leader_still_valid(&list.names[i], valueNumber, scope)) {
            *outLeader = list.names[i];
            return 1;
        }
    return 0;
}

/**
 * @brief Gets the value number of an operand, assigning a fresh one if unseen.
 */
static int svn_value_number_of(Operand op, SVNScope *scope, int *vnCounter) {
    ValueKey key = svn_build_value_key(&op);
    if (key.kind < 0) return -1;

    int vn;
    for (SVNScope *s = scope; s; s = s->parent)
        if (operand_table_get(&s->operandToVn, &key, &vn)) return vn;

    vn = (*vnCounter)++;
    operand_table_set(&scope->operandToVn, &key, vn);
    svn_add_leader_for_value(vn, &op, scope);
    return vn;
}

/**
 * @brief Looks up an expression key and replaces the instruction if possible.
 */
static void svn_lookup_or_insert_expr(IRInstr *instruction, const ExprKey *exprKey,
                                       SVNScope *scope, int *vnCounter) {
    int expressionVN;
    int isFound = 0;
    for (SVNScope *s = scope; s; s = s->parent)
        if (expr_table_get(&s->exprToVn, exprKey, &expressionVN)) { isFound = 1; break; }

    Operand leaderOperand;
    if (isFound && svn_find_valid_leader(expressionVN, scope, &leaderOperand)) {
        instruction->op   = IR_ASSIGN;
        instruction->src1 = leaderOperand;
        instruction->src2 = (Operand){ .kind = OPND_NONE };
        svn_define_value(&instruction->dst, expressionVN, scope);
    } else {
        expressionVN = (*vnCounter)++;
        expr_table_set(&scope->exprToVn, exprKey, expressionVN);
        svn_define_value(&instruction->dst, expressionVN, scope);
    }
}

/**
 * @brief Processes a binary or relational IR instruction.
 */
static void svn_process_binary(IRInstr *instruction, SVNScope *scope, int *vnCounter) {
    int vn1 = svn_value_number_of(instruction->src1, scope, vnCounter);
    int vn2 = svn_value_number_of(instruction->src2, scope, vnCounter);

    /* Normalise commutative operations by sorting VN order. */
    if (ir_is_commutative(instruction->op) && vn1 > vn2) {
        int swapTemp = vn1;
        vn1 = vn2;
        vn2 = swapTemp;
    }

    ExprKey exprKey = svn_build_expr_key((int)instruction->op, vn1, vn2);
    svn_lookup_or_insert_expr(instruction, &exprKey, scope, vnCounter);
}

/**
 * @brief Processes a unary IR instruction.
 */
static void svn_process_unary(IRInstr *instruction, SVNScope *scope, int *vnCounter) {
    int vn1 = svn_value_number_of(instruction->src1, scope, vnCounter);
    ExprKey exprKey = svn_build_expr_key((int)instruction->op, vn1, -1);
    svn_lookup_or_insert_expr(instruction, &exprKey, scope, vnCounter);
}

/**
 * @brief Master instruction dispatcher for SVN processing.
 */
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
        ExprKey exprKey = svn_build_expr_key((int)instruction->op, instruction->src1.data.globalOffset, -1);
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
         instructionIndex < irFunction->blocks[blockIndex].bb.range.end; instructionIndex++) {
        svn_process_instr(&irFunction->instrs[instructionIndex], &currentScope, vnCounter);
    }

    for (int successorIndex = 0; successorIndex < 2; successorIndex++) {
        int targetBlock = irFunction->blocks[blockIndex].bb.succ[successorIndex];
        if (targetBlock >= 0 && !visitedBlocks[targetBlock] && irFunction->blocks[targetBlock].predCount == 1) {
            svn_process_ebb(irFunction, targetBlock, &currentScope, vnCounter, visitedBlocks);
        }
    }

    svn_scope_destroy(&currentScope);
}

/**
 * @brief Public entry point for the Superlocal Value Numbering pass.
 */
void svn_optimize(IRFunction *irFunction) {
    if (!irFunction || irFunction->blockCount == 0) return;

    int vnCounter = 0;
    int *visitedBlocks = calloc((size_t)irFunction->blockCount, sizeof(int));

    for (int blockIndex = 0; blockIndex < irFunction->blockCount; blockIndex++) {
        if (!visitedBlocks[blockIndex] && (blockIndex == 0 || irFunction->blocks[blockIndex].predCount != 1)) {
            svn_process_ebb(irFunction, blockIndex, NULL, &vnCounter, visitedBlocks);
        }
    }

    free(visitedBlocks);
}

