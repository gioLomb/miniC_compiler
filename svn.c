/**
 * @file svn.c
 * @brief Superlocal Value Numbering — implementation.
 *
 * Internal organisation:
 *  1. Data structures   — SVNScope (sheaf of tables), ValueKey, ExprKey, NameList.
 *  2. Scope helpers     — create/destroy scopes, hash function.
 *  3. Value-number API  — svn_valueNumberOf, svn_defineValue, svn_addLeaderForValue.
 *  4. Leader validation — svn_isLeaderStillValid, svn_findValidLeader.
 *  5. Instruction processing — svn_lookupOrInsertExpr, svn_processInstr.
 *  6. CFG traversal     — svn_processEbb, svn_optimize.
 */

#include <stdlib.h>
#include <string.h>
#include "svn.h"
#include "hash_table.h"

/* =========================================================================
 * Constants
 * ========================================================================= */

/** Maximum names (leaders) tracked per value number in one scope chain. */
#define SVN_MAX_NAMES 4

/** Initial capacity of each per-scope hash table (small: most scopes are tiny). */
#define SVN_SCOPE_TABLE_CAPACITY 7

/* =========================================================================
 * Data structures
 * =========================================================================
 *
 * SVNScope — one node in the sheaf-of-tables stack.
 *   operandToVnTable  : Operand → value-number  (which VN does this storage location carry?)
 *   exprToVnTable   : ExprKey → value-number  (which VN does this expression produce?)
 *   leaders : value-number → NameList (which names can represent this VN?)
 *   parent  : enclosing scope (NULL at the EBB root)
 *
 * ValueKey — compact, memcmp-able key for a single IR operand.
 *   Stored in 'operandToVnTable' table; kind field disambiguates the union.
 *
 * ExprKey — compact key for a binary/unary expression.
 *   Stored in 'exprToVnTable' table; commutative operands are canonicalised
 *   (vn1 ≤ vn2) before lookup so "a+b" and "b+a" share a VN.
 *
 * NameList — fixed-size list of up to SVN_MAX_NAMES operands that are
 *   known to hold a given value number at some point in the scope chain.
 * ========================================================================= */

typedef struct SVNScope {
    Hash_Table        *operandToVnTable;   // Operand identity → value number
    Hash_Table        *exprToVnTable;    // Expression shape  → value number
    Hash_Table        *leaders;  // Value number      → NameList
    struct SVNScope   *parent;
} SVNScope;

typedef struct {
    int kind;   // 0=VAR, 1=TEMP, 2=CONST_INT, 3=CONST_FLOAT, -1=other
    union {
        struct { int varLevel; int varOffset; };
        int   tempId;
        int   intVal;
        float floatVal;
    } data;
} ValueKey;

typedef struct {
    int op;       // IROp cast to int
    int vn1;      // value number of first operand (canonicalised for commutative ops)
    int vn2;      // value number of second operand
} ExprKey;

typedef struct {
    int     count;
    Operand names[SVN_MAX_NAMES];
} NameList;

/* =========================================================================
 * Hash function
 * ========================================================================= */

/**
 * @brief FNV-1a hash over raw bytes — used for all three per-scope tables.
 *
 * @param key     Pointer to key bytes.
 * @param keySize Size of the key in bytes.
 * @return        FNV-1a hash value.
 */
static unsigned long svn_hash(const void *key, size_t keySize) {
    const unsigned char *bytes = key;
    unsigned long h = 2166136261UL;
    for (size_t i = 0; i < keySize; i++) {
        h ^= bytes[i];
        h *= 16777619UL;
    }
    return h;
}

/* =========================================================================
 * Scope lifecycle
 * ========================================================================= */

/** Initialise a fresh SVNScope with three empty hash tables. */
static void svn_scopeInit(SVNScope *scope, SVNScope *parent) {
    scope->operandToVnTable  = ht_create(SVN_SCOPE_TABLE_CAPACITY, svn_hash);
    scope->exprToVnTable   = ht_create(SVN_SCOPE_TABLE_CAPACITY, svn_hash);
    scope->leaders = ht_create(SVN_SCOPE_TABLE_CAPACITY, svn_hash);
    scope->parent  = parent;
}

/** Destroy the three hash tables owned by @p scope (does not free the struct itself). */
static inline void svn_scopeDestroy(SVNScope *scope) {
    ht_destroy(scope->operandToVnTable,  NULL);
    ht_destroy(scope->exprToVnTable,   NULL);
    ht_destroy(scope->leaders, NULL);
}

/* =========================================================================
 * Operand helpers
 * ========================================================================= */

/**
 * @brief Fill @p k with the ValueKey corresponding to IR operand @p op.
 *
 * The key is zero-initialised first so that padding bytes do not pollute
 * memcmp-based hash lookups.
 */
static inline void svn_buildValueKey(const Operand *op, ValueKey *k) {
    memset(k, 0, sizeof(*k));
    switch (op->kind) {
    case OPND_VAR:
        k->kind            = 0;
        k->data.varLevel   = op->data.varLevel;
        k->data.varOffset  = op->data.varOffset;
        break;
    case OPND_TEMP:
        k->kind          = 1;
        k->data.tempId   = op->data.tempId;
        break;
    case OPND_CONST_INT:
        k->kind          = 2;
        k->data.intVal   = op->data.intVal;
        break;
    case OPND_CONST_FLOAT:
        k->kind          = 3;
        k->data.floatVal = op->data.floatVal;
        break;
    default:
        k->kind = -1;   // not a trackable storage location
        break;
    }
}

/* =========================================================================
 * Leader management
 * ========================================================================= */

/**
 * @brief Append @p name to the NameList for @p vn in the innermost scope.
 *
 * If a NameList already exists (possibly from a parent scope), it is copied
 * into @p scope before appending so that parent scopes are never mutated.
 * Entries beyond SVN_MAX_NAMES are silently dropped.
 */
static void svn_addLeaderForValue(int vn, const Operand *name, SVNScope *scope) {
    NameList list;
    memset(&list, 0, sizeof list);

    // Walk up to find an existing NameList for this VN (may live in a parent scope).
    for (SVNScope *s = scope; s; s = s->parent) {
        if (ht_get(s->leaders, &vn, sizeof(vn), &list, sizeof(list))) break;
    }

    if (list.count < SVN_MAX_NAMES)
        list.names[list.count++] = *name;

    // Always write into the *current* scope so parent scopes are unchanged.
    ht_set(scope->leaders, &vn, sizeof(vn), &list, sizeof(list));
}

/**
 * @brief Associate @p dst with value number @p vn in @p scope.
 *
 * Only VAR and TEMP operands are tracked; others are ignored.
 */
static void svn_defineValue(const Operand *dst, int vn, SVNScope *scope) {
    if (dst->kind != OPND_VAR && dst->kind != OPND_TEMP) return;
    ValueKey k;
    svn_buildValueKey(dst, &k);
    ht_set(scope->operandToVnTable, &k, sizeof(k), &vn, sizeof(vn));
    svn_addLeaderForValue(vn, dst, scope);
}

/* =========================================================================
 * Leader validation
 * ========================================================================= */

/**
 * @brief Return 1 if @p name still carries @p vn in the current scope chain.
 *
 * Temporaries are always valid (written once).  For variables, the current
 * value number is looked up and compared to @p vn; if the variable has been
 * reassigned since being registered as a leader, it no longer carries the
 * expected value and must be discarded.
 */
static int svn_isLeaderStillValid(const Operand *name, int vn, SVNScope *scope) {
    if (name->kind != OPND_VAR) return 1;   // temporaries never go stale
    ValueKey k;
    svn_buildValueKey(name, &k);
    for (SVNScope *s = scope; s; s = s->parent) {
        int currentVN;
        if (ht_get(s->operandToVnTable, &k, sizeof(k), &currentVN, sizeof(currentVN)))
            return currentVN == vn;
    }
    return 0;
}

/**
 * @brief Find a currently-valid leader for @p vn and write it into @p outLeader.
 *
 * Walks the NameList stored in the scope chain and returns the first name
 * that passes svn_isLeaderStillValid().
 *
 * @return 1 if a valid leader was found, 0 otherwise.
 */
static int svn_findValidLeader(int vn, SVNScope *scope, Operand *outLeader) {
    NameList list;
    memset(&list, 0, sizeof list);
    int listFound = 0;

    for (SVNScope *s = scope; s; s = s->parent) {
        if (ht_get(s->leaders, &vn, sizeof(vn), &list, sizeof(list))) {
            listFound = 1;
            break;
        }
    }
    if (!listFound) return 0;

    for (int i = 0; i < list.count; i++) {
        if (svn_isLeaderStillValid(&list.names[i], vn, scope)) {
            *outLeader = list.names[i];
            return 1;
        }
    }
    return 0;
}

/* =========================================================================
 * Value number lookup / creation
 * ========================================================================= */

/**
 * @brief Return the value number for IR operand @p op, creating one if absent.
 *
 * Searches the scope chain from innermost to outermost.  On miss, a fresh
 * value number is allocated from @p *vnCounter and stored in the current scope.
 *
 * @return Value number, or -1 if @p op is not a trackable kind.
 */
static int svn_valueNumberOf(Operand op, SVNScope *scope, int *vnCounter) {
    ValueKey k;
    svn_buildValueKey(&op, &k);
    if (k.kind < 0) return -1;   // constants/labels have no VN in the table

    int vn;
    for (SVNScope *s = scope; s; s = s->parent) {
        if (ht_get(s->operandToVnTable, &k, sizeof(k), &vn, sizeof(vn))) return vn;
    }

    // First encounter: assign a fresh value number and record it.
    vn = (*vnCounter)++;
    ht_set(scope->operandToVnTable, &k, sizeof(k), &vn, sizeof(vn));
    svn_addLeaderForValue(vn, &op, scope);
    return vn;
}


/* =========================================================================
 * Memoisation and rewriting
 * ========================================================================= */

/**
 * @brief Look up @p ek in the scope chain; rewrite @p in as a copy if found.
 *
 * On a *hit* with a valid leader: rewrites @p in to  dst = leader  (IR_ASSIGN).
 * On a *miss*: assigns a fresh VN to the expression and records it so future
 * identical expressions can be detected.
 *
 * In both cases, svn_defineValue() stamps @p in->dst with the resulting VN so
 * subsequent uses of the destination operand inherit the correct number.
 *
 * @param in    Instruction being processed (possibly modified in place).
 * @param ek    Expression key for the computation in @p in.
 * @param scope Current innermost scope.
 * @param vnCounter Counter for fresh value numbers.
 */
static void svn_lookupOrInsertExpr(IRInstr *in, const ExprKey *ek,
                              SVNScope *scope, int *vnCounter) {
    int  exprVN;
    int  exprVnFound = 0;

    // Check whether this expression was already computed along this EBB path.
    for (SVNScope *s = scope; s; s = s->parent) {
        if (ht_get(s->exprToVnTable, (void *)ek, sizeof(*ek), &exprVN, sizeof(exprVN))) {
            exprVnFound = 1;
            break;
        }
    }

    Operand leader;
    if (exprVnFound && svn_findValidLeader(exprVN, scope, &leader)) {
        // Redundant computation: replace with a copy from the leader.
        in->op   = IR_ASSIGN;
        in->src1 = leader;
        in->src2 = noOperand();
        svn_defineValue(&in->dst, exprVN, scope);
    }else{
        // New expression: record its VN for future de-duplication.
        exprVN = (*vnCounter)++;
        ht_set(scope->exprToVnTable, (void *)ek, sizeof(*ek), &exprVN, sizeof(exprVN));
        svn_defineValue(&in->dst, exprVN, scope);
    }
}

/* =========================================================================
 * Per-instruction processing
 * ========================================================================= */

/**
 * @brief Apply SVN to a single instruction @p in.
 *
 * Dispatches on the opcode:
 *  - Binary / relational / unary: build ExprKey, call svn_lookupOrInsertExpr.
 *  - IR_ASSIGN: propagate the source VN to the destination.
 *  - IR_LOAD_ARR / IR_CALL: assign a fresh VN (not memoised; see svn.h).
 *  - Side-effecting / control-flow ops: no VN bookkeeping needed.
 *
 * @param in     Instruction to process (may be rewritten in place).
 * @param scope  Current innermost scope.
 * @param vnCounter Pointer to the next-free value-number counter.
 */
static void svn_processInstr(IRInstr *in, SVNScope *scope, int *vnCounter) {
    switch (in->op) {
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_LT:  case IR_LE:  case IR_GT:  case IR_GE:  case IR_EQ: case IR_NE: {
        int vn1 = svn_valueNumberOf(in->src1, scope, vnCounter);
        int vn2 = svn_valueNumberOf(in->src2, scope, vnCounter);

        // Canonicalise commutative operands so "a+b" == "b+a".
        if (ir_isCommutative(in->op) && vn1 > vn2) {
            int t = vn1; vn1 = vn2; vn2 = t;
        }

        ExprKey ek;
        memset(&ek, 0, sizeof ek);
        ek.op = (int)in->op;
        ek.vn1 = vn1;
        ek.vn2 = vn2;
        svn_lookupOrInsertExpr(in, &ek, scope, vnCounter);
        break;
    }

    case IR_NEG: case IR_NOT: {
        int vn1 = svn_valueNumberOf(in->src1, scope, vnCounter);

        ExprKey ek;
        memset(&ek, 0, sizeof ek);
        ek.op  = (int)in->op;
        ek.vn1 = vn1;
        ek.vn2 = -1;   // sentinel: unary has no second operand
        svn_lookupOrInsertExpr(in, &ek, scope, vnCounter);
        break;
    }

    case IR_ASSIGN: {
        // Propagate the source's VN to the destination.
        int vn1 = svn_valueNumberOf(in->src1, scope, vnCounter);
        svn_defineValue(&in->dst, vn1, scope);
        break;
    }

    case IR_LOAD_ARR:
    case IR_CALL: {
        // Cannot memoise: alias / side-effect uncertainty. Assign a unique VN.
        int fresh = (*vnCounter)++;
        svn_defineValue(&in->dst, fresh, scope);
        break;
    }

    // Control-flow and side-effecting ops carry no value to track.
    case IR_STORE_ARR:
    case IR_PARAM:
    case IR_RETURN:
    case IR_GOTO:
    case IR_IF_FALSE:
    case IR_LABEL:
        break;
    }
}

/* =========================================================================
 * CFG traversal
 * ========================================================================= */

/**
 * @brief Process one block and its single-predecessor successors (the EBB).
 *
 * Opens a fresh SVNScope as a child of @p parent, processes every instruction
 * in the block, then recursively visits each successor whose predCount == 1
 * (i.e., it belongs to the same EBB).  On return, the scope is destroyed so
 * that sibling branches never see operandToVnTable from this branch.
 *
 * @param f         IR function containing the blocks and instructions.
 * @param blockIdx  Index of the block to process.
 * @param parent    Enclosing scope (NULL at the EBB entry point).
 * @param vnCounter    Shared counter for fresh value numbers.
 * @param visited   Per-block visited flag to avoid re-processing.
 */
static void svn_processEbb(IRFunction *f, int blockIdx, SVNScope *parent,
                             int *vnCounter, int *visited) {
    visited[blockIdx] = 1;

    SVNScope scope;
    svn_scopeInit(&scope, parent);

    // Process each instruction in this block under the current scope.
    for (int i = f->blocks[blockIdx].bb.start;
         i < f->blocks[blockIdx].bb.end; i++)
        svn_processInstr(&f->instrs[i], &scope, vnCounter);

    // Recursively extend the EBB to any successor with a single predecessor.
    for (int succ_idx = 0; succ_idx < 2; succ_idx++) {
        int s = f->blocks[blockIdx].bb.succ[succ_idx];
        if (s >= 0 && !visited[s] && f->blocks[s].predCount == 1)
            svn_processEbb(f, s, &scope, vnCounter, visited);
    }

    svn_scopeDestroy(&scope);
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void svn_optimize(IRFunction *f) {
    if (f->blockCount == 0) return;

    int  vnCounter  = 0;
    int *visited = calloc((size_t)f->blockCount, sizeof(int));

    // Start a new top-level EBB from every block that is either the entry
    // block or a join point (predCount != 1).  Blocks inside an EBB are
    // reached recursively and skipped here via the visited[] flag.
    for (int i = 0; i < f->blockCount; i++) {
        if (!visited[i] && (i == 0 || f->blocks[i].predCount != 1))
            svn_processEbb(f, i, NULL, &vnCounter, visited);
    }

    free(visited);
}
