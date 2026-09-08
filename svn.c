/**
 * @file svn.c
 * @brief Superlocal Value Numbering (SVN) pass implementation.
 *
 * Internal organization
 * ---------------------
 *  svn_hash                 - FNV-1a hash function over raw key bytes.
 *  svn_build_value_key      - Constructs a ValueKey for an IR operand.
 *  svn_build_expr_key       - Constructs an ExprKey for an expression.
 *  svn_scope_init           - Allocates and initialises a scope from an Arena.
 *  svn_scope_destroy        - Destroys scope resources and hash tables.
 *  svn_add_leader_for_value - Adds an operand leader for a value number.
 *  svn_define_value         - Associates a destination operand with a value number.
 *  svn_is_leader_still_valid- Verifies if a leader operand has not been reassigned.
 *  svn_find_valid_leader    - Returns a valid leader operand for a value number.
 *  svn_value_number_of      - Retrieves or assigns a value number for an operand.
 *  svn_lookup_or_insert_expr- Looks up an expression key or assigns a new VN.
 *  svn_process_binary       - Handles binary and relational IR instructions.
 *  svn_process_unary        - Handles unary IR instructions.
 *  svn_process_instr        - Master instruction dispatch handler for SVN.
 *  svn_process_ebb          - Traverses an Extended Basic Block recursively.
 *  svn_optimize             - Public entry point for the SVN pass.
 */

#include <stdlib.h>
#include <string.h>
#include "svn.h"
#include "hash_table.h"
#include "arena.h"



/**
 * @brief Represents a single scope in the sheaf-of-tables hierarchy.
 */
typedef struct SVNScope {
    Hash_Table        *operandToVnTable; /**< Operand identity -> Value Number */
    Hash_Table        *exprToVnTable;    /**< Expression shape  -> Value Number */
    Hash_Table        *leaders;          /**< Value Number      -> NameList */
    struct SVNScope   *parent;           /**< Enclosing parent scope */
} SVNScope;

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
 * @brief FNV-1a non-cryptographic hash over raw bytes.
 *
 * @param key     Pointer to the data to hash.
 * @param keySize Size of the data in bytes.
 * @return Hash value.
 */
static unsigned long svn_hash(const void *key, size_t keySize) {
    const unsigned char *bytes = key;
    unsigned long hashValue = 2166136261UL;
    for (size_t byteIndex = 0; byteIndex < keySize; byteIndex++) {
        hashValue ^= bytes[byteIndex];
        hashValue *= 16777619UL;
    }
    return hashValue;
}

/**
 * @brief Constructs a normalized ValueKey for an IR operand.
 *
 * Normalizes VAR, TEMP, CONST_INT, and CONST_FLOAT operands into a
 * compact key suitable for hash table lookups.
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
        key.kind           = -1;  // Not representable (e.g. NONE, global addr)
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
 * @brief Initialises a new SVN scope.
 *
 * Creates three hash tables: operand->VN, expression->VN, and VN->leaders.
 *
 * @param scope  Scope to initialise.
 * @param parent Parent scope (may be NULL).
 */
static void svn_scope_init(SVNScope *scope, SVNScope *parent) {
    scope->operandToVnTable = ht_create(SVN_SCOPE_TABLE_CAPACITY, svn_hash);
    scope->exprToVnTable    = ht_create(SVN_SCOPE_TABLE_CAPACITY, svn_hash);
    scope->leaders          = ht_create(SVN_SCOPE_TABLE_CAPACITY, svn_hash);
    scope->parent           = parent;
}

/**
 * @brief Destroys an SVN scope and frees its hash tables.
 *
 * @param scope Scope to destroy.
 */
static inline void svn_scope_destroy(SVNScope *scope) {
    ht_destroy(scope->operandToVnTable, NULL);
    ht_destroy(scope->exprToVnTable,    NULL);
    ht_destroy(scope->leaders,          NULL);
}


/**
 * @brief Adds a name (operand) as a leader for a value number.
 *
 * Retrieves the existing NameList from the nearest ancestor scope that
 * contains it, appends the new name, and stores the updated list in the
 * current scope (shadowing ancestors).
 *
 * @param valueNumber Value number to add a leader for.
 * @param name        Operand to add as a leader.
 * @param scope       Current scope (where the list will be stored).
 */
static void svn_add_leader_for_value(int valueNumber, const Operand *name, SVNScope *scope) {
    NameList list;
    memset(&list, 0, sizeof(list));

    // Search ancestor scopes for an existing NameList.
    for (SVNScope *currentScope = scope; currentScope; currentScope = currentScope->parent) {
        if (ht_get(currentScope->leaders, &valueNumber, sizeof(valueNumber), &list, sizeof(list))) {
            break;  // Found existing list; will update it in current scope.
        }
    }

    // Append the new name if there is room.
    if (list.count < SVN_MAX_NAMES) {
        list.names[list.count++] = *name;
    }

    // Store (or update) the list in the current scope, shadowing ancestors.
    ht_set(scope->leaders, &valueNumber, sizeof(valueNumber), &list, sizeof(list));
}

/**
 * @brief Associates a destination operand with a value number.
 *
 * Binds the operand to its VN in the operand->VN table and adds it as a
 * leader for that VN.
 *
 * @param destination Destination operand.
 * @param valueNumber Value number to bind.
 * @param scope       Current scope.
 */
static void svn_define_value(const Operand *destination, int valueNumber, SVNScope *scope) {
    if (destination->kind != OPND_VAR && destination->kind != OPND_TEMP) return;
    
    ValueKey key = svn_build_value_key(destination);
    ht_set(scope->operandToVnTable, &key, sizeof(key), &valueNumber, sizeof(valueNumber));
    svn_add_leader_for_value(valueNumber, destination, scope);
}

/**
 * @brief Checks if a name (variable) still holds a given value number.
 *
 * Searches the scope chain from innermost to outermost for the variable's
 * most recent binding. If found and it matches the expected VN, the name
 * is still valid. Temporaries are always valid (single definition per path).
 *
 * @param name        Operand to check (must be a variable or temporary).
 * @param valueNumber Expected value number.
 * @param scope       Current scope.
 * @return 1 if the name still holds the VN, 0 otherwise.
 */
static int svn_is_leader_still_valid(const Operand *name, int valueNumber, SVNScope *scope) {
    // Temps are defined once per path → always valid.
    if (name->kind != OPND_VAR) return 1;

    ValueKey key = svn_build_value_key(name);
    // Search innermost to outermost: most recent definition wins.
    for (SVNScope *currentScope = scope; currentScope; currentScope = currentScope->parent) {
        int activeValueNumber;
        if (ht_get(currentScope->operandToVnTable, &key, sizeof(key), &activeValueNumber, sizeof(activeValueNumber))) {
            return activeValueNumber == valueNumber;
        }
    }
    // Not defined in any scope → cannot be a valid leader.
    return 0;
}

/**
 * @brief Finds a valid leader operand for a value number.
 *
 * Retrieves the NameList from the nearest ancestor scope and scans it for
 * a name that is still valid in the current context.
 *
 * @param valueNumber Value number to find a leader for.
 * @param scope       Current scope.
 * @param outLeader   Output pointer for the found leader operand.
 * @return 1 if a valid leader was found, 0 otherwise.
 */
static int svn_find_valid_leader(int valueNumber, SVNScope *scope, Operand *outLeader) {
    NameList list;
    memset(&list, 0, sizeof(list));
    int listFound = 0;

    // Retrieve NameList from the nearest ancestor scope that has it.
    for (SVNScope *currentScope = scope; currentScope; currentScope = currentScope->parent) {
        if (ht_get(currentScope->leaders, &valueNumber, sizeof(valueNumber), &list, sizeof(list))) {
            listFound = 1;
            break;
        }
    }
    if (!listFound) return 0;

    // Scan the list and return the first valid name.
    for (int index = 0; index < list.count; index++) {
        if (svn_is_leader_still_valid(&list.names[index], valueNumber, scope)) {
            *outLeader = list.names[index];
            return 1;
        }
    }
    return 0;
}


/**
 * @brief Gets the value number of an operand, assigning a fresh one if unseen.
 *
 * Searches the scope chain for an existing binding. If found, returns it.
 * Otherwise, creates a fresh VN and binds the operand in the current scope.
 *
 * @param op        Operand to number.
 * @param scope     Current scope.
 * @param vnCounter Pointer to the global VN counter (updated when assigning).
 * @return Value number, or -1 for unsupported operand kinds.
 */
static int svn_value_number_of(Operand op, SVNScope *scope, int *vnCounter) {
    ValueKey key = svn_build_value_key(&op);
    if (key.kind < 0) return -1;  // Unsupported operand kind.

    int valueNumber;
    // Search ancestor scopes for an existing binding.
    for (SVNScope *currentScope = scope; currentScope; currentScope = currentScope->parent) {
        if (ht_get(currentScope->operandToVnTable, &key, sizeof(key), &valueNumber, sizeof(valueNumber))) {
            return valueNumber;
        }
    }

    // Not found → create a fresh VN and bind it in the current scope.
    valueNumber = (*vnCounter)++;
    ht_set(scope->operandToVnTable, &key, sizeof(key), &valueNumber, sizeof(valueNumber));
    svn_add_leader_for_value(valueNumber, &op, scope);
    return valueNumber;
}

/**
 * @brief Looks up an expression key and replaces the instruction if possible.
 *
 * Searches ancestor scopes for the expression. If found and a valid leader
 * exists, replaces the instruction with a copy from that leader. Otherwise,
 * assigns a fresh VN and records the expression in the current scope.
 *
 * @param instruction Instruction being processed (may be rewritten).
 * @param exprKey     Expression key to look up.
 * @param scope       Current scope.
 * @param vnCounter   Pointer to the global VN counter.
 */
static void svn_lookup_or_insert_expr(IRInstr *instruction, const ExprKey *exprKey,
                                       SVNScope *scope, int *vnCounter) {
    int expressionVN;
    int isFound = 0;

    // Check all ancestor scopes for the expression key.
    for (SVNScope *currentScope = scope; currentScope; currentScope = currentScope->parent) {
        if (ht_get(currentScope->exprToVnTable, (void *)exprKey, sizeof(*exprKey), &expressionVN, sizeof(expressionVN))) {
            isFound = 1;
            break;
        }
    }

    Operand leaderOperand;
    if (isFound && svn_find_valid_leader(expressionVN, scope, &leaderOperand)) {
        // Replace computation with a copy from a valid leader.
        instruction->op   = IR_ASSIGN;
        instruction->src1 = leaderOperand;
        instruction->src2 = (Operand){ .kind = OPND_NONE };
        svn_define_value(&instruction->dst, expressionVN, scope);
    } else {
        // New expression: assign fresh VN and record in expr table.
        expressionVN = (*vnCounter)++;
        ht_set(scope->exprToVnTable, (void *)exprKey, sizeof(*exprKey), &expressionVN, sizeof(expressionVN));
        svn_define_value(&instruction->dst, expressionVN, scope);
    }
}


/**
 * @brief Processes a binary or relational IR instruction.
 *
 * Computes VNs for both operands, normalises commutative operations by
 * sorting VN order, and looks up or inserts the expression.
 *
 * @param instruction Instruction to process.
 * @param scope       Current scope.
 * @param vnCounter   Pointer to the global VN counter.
 */
static void svn_process_binary(IRInstr *instruction, SVNScope *scope, int *vnCounter) {
    int vn1 = svn_value_number_of(instruction->src1, scope, vnCounter);
    int vn2 = svn_value_number_of(instruction->src2, scope, vnCounter);

    // Normalise commutative operations by sorting VN order.
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
 *
 * Computes VN for the operand and looks up or inserts the expression.
 *
 * @param instruction Instruction to process.
 * @param scope       Current scope.
 * @param vnCounter   Pointer to the global VN counter.
 */
static void svn_process_unary(IRInstr *instruction, SVNScope *scope, int *vnCounter) {
    int vn1 = svn_value_number_of(instruction->src1, scope, vnCounter);
    ExprKey exprKey = svn_build_expr_key((int)instruction->op, vn1, -1);
    svn_lookup_or_insert_expr(instruction, &exprKey, scope, vnCounter);
}

/**
 * @brief Master instruction dispatcher for SVN processing.
 *
 * Routes each instruction to the appropriate handler based on its opcode.
 * Instructions without a destination or those that cannot be value-numbered
 * are simply skipped.
 *
 * @param instruction Instruction to process.
 * @param scope       Current scope.
 * @param vnCounter   Pointer to the global VN counter.
 */
static void svn_process_instr(IRInstr *instruction, SVNScope *scope, int *vnCounter) {
    switch (instruction->op) {
    // Binary arithmetic and relational ops.
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_LT:  case IR_LE:  case IR_GT:  case IR_GE:  case IR_EQ: case IR_NE:
        svn_process_binary(instruction, scope, vnCounter);
        break;

    // Unary ops.
    case IR_NEG: case IR_NOT:
        svn_process_unary(instruction, scope, vnCounter);
        break;

    // Assignment: propagate source VN to destination.
    case IR_ASSIGN: {
        int sourceVN = svn_value_number_of(instruction->src1, scope, vnCounter);
        svn_define_value(&instruction->dst, sourceVN, scope);
        break;
    }

    // Operations that produce fresh values (cannot be safely value-numbered).
    case IR_LOAD_ARR:
    case IR_CALL: {
        int freshVN = (*vnCounter)++;
        svn_define_value(&instruction->dst, freshVN, scope);
        break;
    }

    // Global address is a constant expression.
    case IR_GLOBAL_ADDR: {
        // Key uses global offset as a pseudo-operand.
        ExprKey exprKey = svn_build_expr_key((int)instruction->op, instruction->src1.data.globalOffset, -1);
        svn_lookup_or_insert_expr(instruction, &exprKey, scope, vnCounter);
        break;
    }

    // Instructions with no destination, or side-effect ops not value-numbered.
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
 *
 * Traverses a maximal sequence of blocks where each block has exactly one
 * predecessor, sharing value numbering information within the EBB.
 *
 * @param irFunction    IR function being processed.
 * @param blockIndex    Current block index.
 * @param parentScope   Parent scope (inherited from previous blocks).
 * @param vnCounter     Pointer to the global VN counter.
 * @param visitedBlocks Array marking which blocks have been visited.
 */
static void svn_process_ebb(IRFunction *irFunction, int blockIndex, SVNScope *parentScope,
                            int *vnCounter, int *visitedBlocks) {
    // Mark block as visited.
    visitedBlocks[blockIndex] = 1;

    // Create a new scope for this block, inheriting from parent.
    SVNScope currentScope;
    svn_scope_init(&currentScope, parentScope);

    // Process all instructions in the block.
    for (int instructionIndex = irFunction->blocks[blockIndex].bb.range.start;
         instructionIndex < irFunction->blocks[blockIndex].bb.range.end; instructionIndex++) {
        svn_process_instr(&irFunction->instrs[instructionIndex], &currentScope, vnCounter);
    }

    // Recurse into successors that have only one predecessor (EBB condition).
    for (int successorIndex = 0; successorIndex < 2; successorIndex++) {
        int targetBlock = irFunction->blocks[blockIndex].bb.succ[successorIndex];
        if (targetBlock >= 0 && !visitedBlocks[targetBlock] && irFunction->blocks[targetBlock].predCount == 1) {
            svn_process_ebb(irFunction, targetBlock, &currentScope, vnCounter, visitedBlocks);
        }
    }

    // Destroy the scope after processing all children.
    svn_scope_destroy(&currentScope);
}


/**
 * @brief Public entry point for the Superlocal Value Numbering pass.
 *
 * Identifies equivalent computations within Extended Basic Blocks and
 * replaces them with copies from existing leaders. This reduces redundant
 * computations and enables further optimizations.
 *
 * @param irFunction IR function to optimize.
 */
void svn_optimize(IRFunction *irFunction) {
    if (!irFunction || irFunction->blockCount == 0) return;

    int vnCounter = 0;
    int *visitedBlocks = calloc((size_t)irFunction->blockCount, sizeof(int));

    // Start a new EBB at every block that is either the entry or has >1 predecessors.
    for (int blockIndex = 0; blockIndex < irFunction->blockCount; blockIndex++) {
        if (!visitedBlocks[blockIndex] && (blockIndex == 0 || irFunction->blocks[blockIndex].predCount != 1)) {
            svn_process_ebb(irFunction, blockIndex, NULL, &vnCounter, visitedBlocks);
        }
    }

    free(visitedBlocks);
}